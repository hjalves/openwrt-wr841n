/*
 * lanwatch: list the devices on the LAN, like a tiny "Fing", for a small
 * OpenWrt access point. Exposed on ubus as "lanwatch".
 *
 * Discovery:
 *  - passive: a raw socket on the LAN bridge with a BPF filter for ARP,
 *    DHCP (hostname, vendor class), mDNS (names, reverse answers, device
 *    model), SSDP (SERVER header) and NetBIOS replies;
 *  - active: a slow ARP sweep of the subnet every few minutes, then name
 *    queries (NetBIOS node status, unicast mDNS reverse lookup) for devices
 *    without a name, at most once an hour each;
 *  - this access point's own Wi-Fi clients (signal) via libiwinfo.
 *
 * Every received frame is untrusted: all parsing is bounds-checked and
 * names are reduced to printable ASCII.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/filter.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifndef NO_IWINFO
#include <iwinfo.h>
#endif
#include <libubus.h>
#include <libubox/blobmsg.h>
#include <libubox/uloop.h>

#define MAX_DEV        256
#define NAME_LEN       48
#define INFO_LEN       64
#define SWEEP_GAP_MS   20       /* between ARP requests in a sweep */
#define NAME_RETRY     3600     /* seconds between name queries per device */
#define NEW_SECONDS    86400    /* "new" = first seen within a day */
#define SAVE_MIN_GAP   600      /* at most one save per 10 min for new devices */
#define STATE_MAGIC    0x4c574456u /* "LWDV" */
#define STATE_VERSION  2u  /* v2: renumbered name/info sources */

/* How a device was seen (bit flags). */
enum { V_ARP = 1, V_DHCP = 2, V_MDNS = 4, V_SSDP = 8, V_NBNS = 16, V_WIFI = 32, V_SELF = 64 };
static const char *const via_names[] = { "arp", "dhcp", "mdns", "ssdp", "netbios", "wifi", "self" };

/* Name sources, in increasing priority. */
enum { N_NONE, N_MDNS_ID, N_NBNS, N_MDNS_REV, N_MDNS, N_CAST, N_DHCP, N_SELF };
static const char *const name_src_names[] = { "", "mdns-id", "netbios", "mdns-reverse", "mdns", "cast", "dhcp", "self" };

/* Info sources, in increasing priority. */
enum { I_NONE, I_GATEWAY, I_DHCP, I_SSDP, I_MODEL, I_SELF };
static const char *const info_src_names[] = { "", "route", "dhcp-vendor", "ssdp", "mdns-model", "self" };

struct dev {
	uint8_t mac[6];
	uint8_t name_src, info_src;
	uint32_t ip;              /* network order, 0 = unknown */
	uint32_t first_seen, last_seen;
	uint32_t name_tried;      /* last name query sent */
	uint8_t via;
	int8_t signal;            /* dBm on our Wi-Fi, 0 = not associated here */
	uint8_t vendor;           /* vendor table index + 1; 0 = none or not known */
	uint8_t pad;
	char name[NAME_LEN];
	char info[INFO_LEN];
};

/* ---- configuration ---- */
static const char *cfg_if = "br-lan";
static const char *cfg_wifi = "wlan0";
static const char *cfg_state;               /* NULL = no persistence */
static const char *cfg_oui = "/usr/share/lanwatch/oui.bin";
static unsigned cfg_sweep = 300;            /* seconds */
static const char *cfg_socket;

/* ---- state ---- */
static struct dev devs[MAX_DEV];
static unsigned ndev;
static int ifindex;
static uint8_t my_mac[6];
static uint32_t my_ip, my_mask;             /* network order */
static uint32_t last_save, unsaved_new;
static bool dirty;
static uint32_t sweeps, sweep_last;
static unsigned frames_seen;

static uint32_t now_s(void)
{
	return (uint32_t)time(NULL);
}

/* ------------------------------------------------------------------ */
/* Text helpers                                                       */
/* ------------------------------------------------------------------ */

/* Copy at most n bytes of untrusted text, keeping printable ASCII only. */
static void clean_copy(char *dst, size_t size, const uint8_t *src, size_t n)
{
	size_t i, o = 0;

	for (i = 0; i < n && o + 1 < size; i++) {
		uint8_t c = src[i];

		if (c == 0)
			break;
		dst[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
	}
	while (o && dst[o - 1] == ' ')
		o--;
	dst[o] = '\0';
}

static bool mac_valid(const uint8_t *m)
{
	static const uint8_t zero[6];

	return !(m[0] & 1) && memcmp(m, zero, 6) && memcmp(m, my_mac, 6);
}

static bool ip_in_lan(uint32_t ip)
{
	return ip && my_mask && (ip & my_mask) == (my_ip & my_mask) && ip != my_ip &&
	       ip != ((my_ip & my_mask) | ~my_mask) && ip != (my_ip & my_mask);
}

/* ------------------------------------------------------------------ */
/* Vendor table (see tools/mkoui.py)                                  */
/* ------------------------------------------------------------------ */

static uint8_t *oui_buf;
static size_t oui_len, oui_entries_off;
static uint32_t oui_count;
static uint16_t oui_name_off[256];
static unsigned oui_nv;

/* Load and validate the table once; on any problem vendors stay unknown. */
static void oui_load(void)
{
	FILE *f = fopen(cfg_oui, "rb");
	size_t off;
	unsigned v;

	if (!f)
		return;
	oui_buf = malloc(64 * 1024);
	if (oui_buf)
		oui_len = fread(oui_buf, 1, 64 * 1024, f);
	fclose(f);
	if (!oui_buf || oui_len < 12 || memcmp(oui_buf, "LWOUI1", 6))
		goto bad;
	oui_nv = oui_buf[6] | oui_buf[7] << 8;
	if (oui_nv > 255)
		goto bad;
	for (off = 8, v = 0; v < oui_nv; v++) {
		if (off >= oui_len || off + 1 + oui_buf[off] > oui_len)
			goto bad;
		oui_name_off[v] = (uint16_t)off;
		off += 1 + oui_buf[off];
	}
	if (off + 4 > oui_len)
		goto bad;
	oui_count = (uint32_t)oui_buf[off] | (uint32_t)oui_buf[off + 1] << 8 |
	            (uint32_t)oui_buf[off + 2] << 16 | (uint32_t)oui_buf[off + 3] << 24;
	oui_entries_off = off + 4;
	return;
bad:
	syslog(LOG_WARNING, "vendor table %s unusable; vendors unknown", cfg_oui);
	free(oui_buf);
	oui_buf = NULL;
	oui_nv = 0;
}

/* Vendor index + 1 for this MAC, or 0. */
static uint8_t oui_lookup(const uint8_t *mac)
{
	uint32_t want = (uint32_t)mac[0] << 16 | mac[1] << 8 | mac[2], cur = 0, i;
	size_t off = oui_entries_off;

	if (!oui_buf || (mac[0] & 2))
		return 0;  /* no table, or a locally administered (random) MAC */
	for (i = 0; i < oui_count && off < oui_len; i++) {
		uint32_t d = 0;
		int sh = 0;
		uint8_t v;

		while (off < oui_len && sh < 32) {
			uint8_t b = oui_buf[off++];

			d |= (uint32_t)(b & 0x7f) << sh;
			sh += 7;
			if (!(b & 0x80))
				break;
		}
		if (off >= oui_len)
			return 0;
		cur += d;
		v = oui_buf[off++];
		if (cur > want)
			return 0;  /* sorted: passed it */
		if (cur == want)
			return v < oui_nv ? (uint8_t)(v + 1) : 0;
	}
	return 0;
}

static void oui_name(uint8_t idx, char *out, size_t size)
{
	size_t p;

	out[0] = '\0';
	if (!idx || idx > oui_nv)
		return;
	p = oui_name_off[idx - 1];
	clean_copy(out, size, oui_buf + p + 1, oui_buf[p]);
}

/* ------------------------------------------------------------------ */
/* Device table                                                       */
/* ------------------------------------------------------------------ */

static struct dev *dev_find(const uint8_t *mac)
{
	unsigned i;

	for (i = 0; i < ndev; i++)
		if (!memcmp(devs[i].mac, mac, 6))
			return &devs[i];
	return NULL;
}

static struct dev *dev_by_ip(uint32_t ip)
{
	unsigned i;
	struct dev *best = NULL;

	for (i = 0; i < ndev; i++)
		if (devs[i].ip == ip && (!best || devs[i].last_seen > best->last_seen))
			best = &devs[i];
	return best;
}

/* A free slot, or (table full) the device seen longest ago, never the router itself. */
static struct dev *dev_alloc(void)
{
	unsigned i, oldest = MAX_DEV;
	struct dev *d;

	if (ndev < MAX_DEV) {
		d = &devs[ndev++];
	} else {
		for (i = 0; i < ndev; i++)
			if (!(devs[i].via & V_SELF) && (oldest == MAX_DEV || devs[i].last_seen < devs[oldest].last_seen))
				oldest = i;
		d = &devs[oldest];
	}
	memset(d, 0, sizeof(*d));
	return d;
}

/* Find or add the device with this MAC; marks it seen now. */
static struct dev *dev_seen(const uint8_t *mac, uint8_t via)
{
	struct dev *d = dev_find(mac);
	uint32_t now = now_s();

	if (!d) {
		d = dev_alloc();
		memcpy(d->mac, mac, 6);
		d->first_seen = now;
		d->vendor = oui_lookup(mac);
		syslog(LOG_INFO, "new device %02x:%02x:%02x:%02x:%02x:%02x",
		       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		unsaved_new++;
	}
	d->last_seen = now;
	d->via |= via;
	dirty = true;
	return d;
}

static void dev_set_ip(struct dev *d, uint32_t ip)
{
	if (ip_in_lan(ip))
		d->ip = ip;
}

/* "c9a655a3-8cf0-b0f7-d50c-9b7e9cf7d00d": a random identifier, not a name. */
static bool looks_like_uuid(const char *s)
{
	int i;

	for (i = 0; i < 36; i++) {
		bool dash = i == 8 || i == 13 || i == 18 || i == 23;

		if (dash ? s[i] != '-' : !isxdigit((unsigned char)s[i]))
			return false;
	}
	return s[36] == '\0' || s[36] == '.';
}

static void dev_set_name(struct dev *d, const char *name, uint8_t src)
{
	char clean[NAME_LEN];
	size_t l;

	clean_copy(clean, sizeof(clean), (const uint8_t *)name, strlen(name));
	l = strlen(clean);
	if (l > 6 && !strcasecmp(clean + l - 6, ".local"))
		clean[l - 6] = '\0';
	if (src == N_MDNS && looks_like_uuid(clean))
		src = N_MDNS_ID;
	if (!clean[0] || src < d->name_src)
		return;
	if (strcmp(d->name, clean) || d->name_src != src) {
		snprintf(d->name, sizeof(d->name), "%s", clean);
		d->name_src = src;
		dirty = true;
	}
}

static void dev_set_info(struct dev *d, const char *info, uint8_t src)
{
	char clean[INFO_LEN];

	clean_copy(clean, sizeof(clean), (const uint8_t *)info, strlen(info));
	if (!clean[0] || src < d->info_src)
		return;
	snprintf(d->info, sizeof(d->info), "%s", clean);
	d->info_src = src;
}

/* ------------------------------------------------------------------ */
/* Persistence: whole table, written atomically, rarely              */
/* ------------------------------------------------------------------ */

typedef char dev_size_check[sizeof(struct dev) == 140 ? 1 : -1];

static void state_save(void)
{
	char tmp[PATH_MAX];
	uint32_t hdr[3] = { STATE_MAGIC, STATE_VERSION, ndev };
	FILE *f;

	if (!cfg_state || snprintf(tmp, sizeof(tmp), "%s.tmp", cfg_state) >= (int)sizeof(tmp))
		return;
	if (!(f = fopen(tmp, "wb"))) {
		syslog(LOG_WARNING, "cannot write %s: %s", tmp, strerror(errno));
		return;
	}
	if (fwrite(hdr, sizeof(hdr), 1, f) != 1 || fwrite(devs, sizeof(devs[0]), ndev, f) != ndev ||
	    fflush(f) != 0 || fsync(fileno(f)) != 0) {
		fclose(f);
		unlink(tmp);
		syslog(LOG_WARNING, "write to %s failed", tmp);
		return;
	}
	fclose(f);
	if (rename(tmp, cfg_state) != 0) {
		unlink(tmp);
		return;
	}
	last_save = now_s();
	unsaved_new = 0;
	dirty = false;
}

static void state_load(void)
{
	uint32_t hdr[3];
	FILE *f;
	unsigned i;

	if (!cfg_state || !(f = fopen(cfg_state, "rb")))
		return;
	if (fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == STATE_MAGIC && hdr[1] == STATE_VERSION) {
		ndev = (unsigned)fread(devs, sizeof(devs[0]), hdr[2] < MAX_DEV ? hdr[2] : MAX_DEV, f);
		for (i = 0; i < ndev; i++) {  /* never trust stored text blindly */
			devs[i].name[NAME_LEN - 1] = devs[i].info[INFO_LEN - 1] = '\0';
			devs[i].signal = 0;
			devs[i].via &= (uint8_t)~(V_WIFI | V_SELF);
			devs[i].vendor = oui_lookup(devs[i].mac);  /* table may have changed */
		}
		syslog(LOG_INFO, "loaded %u devices from %s", ndev, cfg_state);
	}
	fclose(f);
	last_save = now_s();
}

/* ------------------------------------------------------------------ */
/* DNS (mDNS) parsing                                                  */
/* ------------------------------------------------------------------ */

/* Decode a possibly compressed name into dotted text. Pointers must go back. */
static bool dns_name(const uint8_t *p, size_t len, size_t *off, char *out, size_t size)
{
	size_t pos = *off, next = 0, o = 0;
	bool jumped = false;

	for (;;) {
		uint8_t l;

		if (pos >= len)
			return false;
		l = p[pos];
		if ((l & 0xc0) == 0xc0) {
			size_t t;

			if (pos + 1 >= len)
				return false;
			t = (size_t)(l & 0x3f) << 8 | p[pos + 1];
			if (t >= pos)
				return false;
			if (!jumped) {
				next = pos + 2;
				jumped = true;
			}
			pos = t;
			continue;
		}
		if (l & 0xc0 || pos + 1 + l > len)
			return false;
		if (l == 0) {
			pos++;
			break;
		}
		if (o && o + 1 < size)
			out[o++] = '.';
		if (o + l >= size)
			return false;
		clean_copy(out + o, size - o, p + pos + 1, l);
		o += strlen(out + o);
		pos += 1 + l;
	}
	out[o] = '\0';
	*off = jumped ? next : pos;
	return true;
}

static void parse_mdns(const uint8_t *p, size_t len, uint32_t src_ip, const uint8_t *src_mac)
{
	unsigned qd, an, ns, ar, i, total;
	size_t off = 12;
	char name[256], target[256];
	struct dev *d;

	if (len < 12 || !(p[2] & 0x80))
		return;  /* responses only */
	qd = (unsigned)(p[4] << 8 | p[5]);
	an = (unsigned)(p[6] << 8 | p[7]);
	ns = (unsigned)(p[8] << 8 | p[9]);
	ar = (unsigned)(p[10] << 8 | p[11]);
	for (i = 0; i < qd; i++)
		if (!dns_name(p, len, &off, name, sizeof(name)) || (off += 4) > len)
			return;
	total = an + ns + ar;
	if (total > 200)
		return;
	for (i = 0; i < total; i++) {
		uint16_t type, rdlen;
		size_t rd;

		if (!dns_name(p, len, &off, name, sizeof(name)) || off + 10 > len)
			return;
		type = (uint16_t)(p[off] << 8 | p[off + 1]);
		rdlen = (uint16_t)(p[off + 8] << 8 | p[off + 9]);
		off += 10;
		rd = off;
		if (off + rdlen > len)
			return;
		off += rdlen;
		if (type == 1 && rdlen == 4) {  /* A: name -> address */
			uint32_t a;

			memcpy(&a, p + rd, 4);
			if (a == src_ip && mac_valid(src_mac) && ip_in_lan(a)) {
				d = dev_seen(src_mac, V_MDNS);
				dev_set_ip(d, a);
				dev_set_name(d, name, N_MDNS);
			}
		} else if (type == 12) {  /* PTR: reverse answers name an address */
			unsigned b[4];
			size_t t = rd;
			char tail[16];

			if (sscanf(name, "%3u.%3u.%3u.%3u.%15s", &b[3], &b[2], &b[1], &b[0], tail) == 5 &&
			    !strcasecmp(tail, "in-addr.arpa") && b[0] < 256 && b[1] < 256 && b[2] < 256 &&
			    b[3] < 256 && dns_name(p, len, &t, target, sizeof(target))) {
				uint32_t a = htonl(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);

				if ((d = dev_by_ip(a)))
					dev_set_name(d, target, N_MDNS_REV);
			}
		} else if (type == 16 && strstr(name, "._googlecast._tcp") && mac_valid(src_mac) &&
		           ip_in_lan(src_ip)) {  /* Cast: fn=friendly name, md=model */
			size_t q = rd;

			while (q < rd + rdlen) {
				uint8_t l = p[q];
				char val[INFO_LEN];

				if (q + 1 + l > rd + rdlen)
					break;
				if (l > 3 && (!memcmp(p + q + 1, "fn=", 3) || !memcmp(p + q + 1, "md=", 3))) {
					clean_copy(val, sizeof(val), p + q + 4, l - 3);
					d = dev_seen(src_mac, V_MDNS);
					dev_set_ip(d, src_ip);
					if (p[q + 1] == 'f')
						dev_set_name(d, val, N_CAST);
					else
						dev_set_info(d, val, I_MODEL);
				}
				q += 1 + l;
			}
		} else if (type == 16 && strstr(name, "._device-info._tcp")) {  /* TXT model=... */
			size_t q = rd;

			while (q < rd + rdlen) {
				uint8_t l = p[q];

				if (q + 1 + l > rd + rdlen)
					break;
				if (l > 6 && !memcmp(p + q + 1, "model=", 6) && mac_valid(src_mac) && ip_in_lan(src_ip)) {
					char model[INFO_LEN];

					clean_copy(model, sizeof(model), p + q + 7, l - 6);
					d = dev_seen(src_mac, V_MDNS);
					dev_set_ip(d, src_ip);
					dev_set_info(d, model, I_MODEL);
				}
				q += 1 + l;
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* DHCP, SSDP, NetBIOS                                                */
/* ------------------------------------------------------------------ */

static void parse_dhcp(const uint8_t *p, size_t len)
{
	const uint8_t *chaddr;
	uint32_t yiaddr, ciaddr, req = 0;
	char host[NAME_LEN] = "", vendor[INFO_LEN] = "";
	size_t off = 240;
	uint8_t mtype = 0;
	struct dev *d;

	if (len < 240 || p[1] != 1 || p[2] != 6 || memcmp(p + 236, "\x63\x82\x53\x63", 4))
		return;  /* Ethernet BOOTP with the DHCP magic cookie */
	chaddr = p + 28;
	memcpy(&ciaddr, p + 12, 4);
	memcpy(&yiaddr, p + 16, 4);
	while (off < len && p[off] != 255) {
		uint8_t opt = p[off], l;

		if (opt == 0) {
			off++;
			continue;
		}
		if (off + 1 >= len || off + 2 + (l = p[off + 1]) > len)
			break;
		if (opt == 12)
			clean_copy(host, sizeof(host), p + off + 2, l);
		else if (opt == 60)
			clean_copy(vendor, sizeof(vendor), p + off + 2, l);
		else if (opt == 50 && l == 4)
			memcpy(&req, p + off + 2, 4);
		else if (opt == 53 && l == 1)
			mtype = p[off + 2];
		off += 2 + l;
	}
	if (!mac_valid(chaddr))
		return;
	if (p[0] == 1) {  /* client -> server: the client describes itself */
		d = dev_seen(chaddr, V_DHCP);
		dev_set_ip(d, ciaddr ? ciaddr : req);
		if (host[0])
			dev_set_name(d, host, N_DHCP);
		if (vendor[0])
			dev_set_info(d, vendor, I_DHCP);
	} else if (p[0] == 2 && mtype == 5 && yiaddr) {  /* server ACK: the address it got */
		d = dev_find(chaddr);
		if (d)
			dev_set_ip(d, yiaddr);
	}
}

static void parse_ssdp(const uint8_t *p, size_t len, uint32_t src_ip, const uint8_t *src_mac)
{
	const uint8_t *line = p, *end = p + len;
	struct dev *d;

	if (!mac_valid(src_mac) || !ip_in_lan(src_ip))
		return;
	d = dev_seen(src_mac, V_SSDP);
	dev_set_ip(d, src_ip);
	while (line < end) {
		const uint8_t *nl = memchr(line, '\n', (size_t)(end - line));
		size_t l = nl ? (size_t)(nl - line) : (size_t)(end - line);

		if (l > 7 && !strncasecmp((const char *)line, "SERVER:", 7)) {
			const uint8_t *v = line + 7;
			char server[INFO_LEN];

			size_t vl;

			while (v < line + l && *v == ' ')
				v++;
			vl = (size_t)(line + l - v);
			if (vl && v[vl - 1] == '\r')
				vl--;  /* strip CR before cleaning, or it becomes '?' */
			clean_copy(server, sizeof(server), v, vl);
			dev_set_info(d, server, I_SSDP);
			break;
		}
		if (!nl)
			break;
		line = nl + 1;
	}
}

/* NetBIOS node status response: the first unique name with suffix 0x00. */
static void parse_nbns(const uint8_t *p, size_t len, uint32_t src_ip, const uint8_t *src_mac)
{
	size_t off = 12;
	unsigned n, i;
	char name[NAME_LEN];
	struct dev *d;

	if (len < 12 + 34 + 10 || !(p[2] & 0x80) || (p[6] << 8 | p[7]) < 1)
		return;
	if ((p[off] & 0xc0) == 0xc0)
		off += 2;  /* name given as a pointer to the question */
	else if (p[off] == 32 && off + 34 <= len)
		off += 34;  /* the "*" name: 32 encoded bytes + root */
	else
		return;
	if (off + 10 > len || p[off] != 0 || p[off + 1] != 0x21)
		return;  /* NBSTAT */
	off += 10;
	if (off >= len)
		return;
	n = p[off++];
	for (i = 0; i < n && off + 18 <= len; i++, off += 18) {
		bool group = (p[off + 16] & 0x80) != 0;

		if (p[off + 15] == 0x00 && !group) {
			clean_copy(name, sizeof(name), p + off, 15);
			if (!mac_valid(src_mac) || !ip_in_lan(src_ip) || !name[0])
				return;
			d = dev_seen(src_mac, V_NBNS);
			dev_set_ip(d, src_ip);
			dev_set_name(d, name, N_NBNS);
			return;
		}
	}
}

static void parse_arp(const uint8_t *p, size_t len)
{
	uint32_t sip;
	struct dev *d;

	if (len < 28 || p[0] != 0 || p[1] != 1 || p[2] != 8 || p[3] != 0 || p[4] != 6 || p[5] != 4)
		return;
	if (!mac_valid(p + 8))
		return;
	memcpy(&sip, p + 14, 4);
	d = dev_seen(p + 8, V_ARP);
	dev_set_ip(d, sip);  /* 0.0.0.0 in ARP probes: MAC only */
}

/* ------------------------------------------------------------------ */
/* Raw socket                                                         */
/* ------------------------------------------------------------------ */

/* tcpdump -dd 'arp or (ip and udp and (port 67 or port 68 or port 5353 or port 1900 or port 137))' */
static struct sock_filter bpf_code[] = {
	{ 0x28, 0, 0, 0x0000000c }, { 0x15, 18, 0, 0x00000806 }, { 0x15, 0, 18, 0x00000800 },
	{ 0x30, 0, 0, 0x00000017 }, { 0x15, 0, 16, 0x00000011 }, { 0x28, 0, 0, 0x00000014 },
	{ 0x45, 14, 0, 0x00001fff }, { 0xb1, 0, 0, 0x0000000e }, { 0x48, 0, 0, 0x0000000e },
	{ 0x15, 10, 0, 0x00000043 }, { 0x15, 9, 0, 0x00000044 }, { 0x15, 8, 0, 0x000014e9 },
	{ 0x15, 7, 0, 0x0000076c }, { 0x15, 6, 0, 0x00000089 }, { 0x48, 0, 0, 0x00000010 },
	{ 0x15, 4, 0, 0x00000043 }, { 0x15, 3, 0, 0x00000044 }, { 0x15, 2, 0, 0x000014e9 },
	{ 0x15, 1, 0, 0x0000076c }, { 0x15, 0, 1, 0x00000089 }, { 0x6, 0, 0, 0x00040000 },
	{ 0x6, 0, 0, 0x00000000 },
};

static struct uloop_fd rawfd = { .fd = -1 };
static int udpfd = -1;
static struct uloop_fd udpu = { .fd = -1 };

static void handle_frame(const uint8_t *f, size_t len)
{
	uint16_t etype;
	const uint8_t *src_mac = f + 6, *ip, *udp, *pl;
	size_t ihl, iplen, pllen;
	uint16_t sport, dport;
	uint32_t src_ip;

	if (len < 14)
		return;
	frames_seen++;
	etype = (uint16_t)(f[12] << 8 | f[13]);
	if (etype == 0x0806) {
		parse_arp(f + 14, len - 14);
		return;
	}
	if (etype != 0x0800 || len < 14 + 20)
		return;
	ip = f + 14;
	ihl = (size_t)(ip[0] & 0x0f) * 4;
	iplen = (size_t)(ip[2] << 8 | ip[3]);
	if ((ip[0] >> 4) != 4 || ihl < 20 || ip[9] != 17 || iplen < ihl + 8 || 14 + iplen > len)
		return;
	if ((ip[6] & 0x3f) || ip[7])
		return;  /* fragments */
	memcpy(&src_ip, ip + 12, 4);
	udp = ip + ihl;
	sport = (uint16_t)(udp[0] << 8 | udp[1]);
	dport = (uint16_t)(udp[2] << 8 | udp[3]);
	pl = udp + 8;
	pllen = iplen - ihl - 8;
	if (sport == 68 && dport == 67)
		parse_dhcp(pl, pllen);
	else if (sport == 67 && dport == 68)
		parse_dhcp(pl, pllen);
	else if (sport == 5353)
		parse_mdns(pl, pllen, src_ip, src_mac);
	else if (sport == 1900 || dport == 1900)
		parse_ssdp(pl, pllen, src_ip, src_mac);
	else if (sport == 137)
		parse_nbns(pl, pllen, src_ip, src_mac);
}

static void raw_cb(struct uloop_fd *fd, unsigned int events)
{
	uint8_t buf[2048];
	struct sockaddr_ll from;
	socklen_t fl;
	ssize_t n;

	(void)events;
	for (;;) {
		fl = sizeof(from);
		n = recvfrom(fd->fd, buf, sizeof(buf), MSG_DONTWAIT | MSG_TRUNC,
		             (struct sockaddr *)&from, &fl);
		if (n < 0)
			return;
		if (from.sll_pkttype == PACKET_OUTGOING || (size_t)n > sizeof(buf))
			continue;  /* our own, or truncated */
		handle_frame(buf, (size_t)n);
	}
}

/* Replies to our name queries are read from the raw socket; drain this one. */
static void udp_cb(struct uloop_fd *fd, unsigned int events)
{
	uint8_t buf[1500];

	(void)events;
	while (recv(fd->fd, buf, sizeof(buf), MSG_DONTWAIT) > 0)
		;
}

static bool if_info(void)
{
	struct ifreq r;
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	bool ok = false;

	if (s < 0)
		return false;
	memset(&r, 0, sizeof(r));
	snprintf(r.ifr_name, sizeof(r.ifr_name), "%s", cfg_if);
	if (ioctl(s, SIOCGIFHWADDR, &r) == 0) {
		memcpy(my_mac, r.ifr_hwaddr.sa_data, 6);
		if (ioctl(s, SIOCGIFADDR, &r) == 0) {
			my_ip = ((struct sockaddr_in *)&r.ifr_addr)->sin_addr.s_addr;
			if (ioctl(s, SIOCGIFNETMASK, &r) == 0) {
				my_mask = ((struct sockaddr_in *)&r.ifr_netmask)->sin_addr.s_addr;
				ok = true;
			}
		}
	}
	close(s);
	return ok;
}

static bool raw_open(void)
{
	struct sock_fprog prog = { sizeof(bpf_code) / sizeof(bpf_code[0]), bpf_code };
	struct sockaddr_ll sll;

	if (rawfd.fd >= 0) {
		uloop_fd_delete(&rawfd);
		close(rawfd.fd);
	}
	ifindex = (int)if_nametoindex(cfg_if);
	if (!ifindex)
		return false;
	rawfd.fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
	if (rawfd.fd < 0) {
		syslog(LOG_ERR, "packet socket: %s", strerror(errno));
		return false;
	}
	if (setsockopt(rawfd.fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog)) < 0)
		syslog(LOG_WARNING, "BPF filter: %s", strerror(errno));
	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_ALL);
	sll.sll_ifindex = ifindex;
	if (bind(rawfd.fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
		syslog(LOG_ERR, "bind %s: %s", cfg_if, strerror(errno));
		close(rawfd.fd);
		rawfd.fd = -1;
		return false;
	}
	/* Receive all multicast (mDNS, SSDP) even for groups nobody on this
	 * host joined; lasts only while this socket is open. */
	{
		struct packet_mreq mr;

		memset(&mr, 0, sizeof(mr));
		mr.mr_ifindex = ifindex;
		mr.mr_type = PACKET_MR_ALLMULTI;
		if (setsockopt(rawfd.fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr)) < 0)
			syslog(LOG_WARNING, "all-multicast on %s: %s", cfg_if, strerror(errno));
	}
	rawfd.cb = raw_cb;
	uloop_fd_add(&rawfd, ULOOP_READ);
	return true;
}

/* ------------------------------------------------------------------ */
/* Active discovery                                                   */
/* ------------------------------------------------------------------ */

static void send_arp(uint32_t target)
{
	uint8_t f[42];
	struct sockaddr_ll to;

	memset(f, 0xff, 6);
	memcpy(f + 6, my_mac, 6);
	f[12] = 0x08; f[13] = 0x06;
	f[14] = 0; f[15] = 1; f[16] = 8; f[17] = 0; f[18] = 6; f[19] = 4; f[20] = 0; f[21] = 1;
	memcpy(f + 22, my_mac, 6);
	memcpy(f + 28, &my_ip, 4);
	memset(f + 32, 0, 6);
	memcpy(f + 38, &target, 4);
	memset(&to, 0, sizeof(to));
	to.sll_family = AF_PACKET;
	to.sll_ifindex = ifindex;
	to.sll_halen = 6;
	memset(to.sll_addr, 0xff, 6);
	sendto(rawfd.fd, f, sizeof(f), 0, (struct sockaddr *)&to, sizeof(to));
}

static void send_name_queries(struct dev *d)
{
	static const uint8_t nbstat[] = {
		0x4c, 0x57, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x20, 'C', 'K', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A',
		'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 0x00,
		0x00, 0x21, 0x00, 0x01,
	};
	uint8_t q[64];
	struct sockaddr_in to;
	const uint8_t *b = (const uint8_t *)&d->ip;
	char rev[40], *lab;
	size_t n = 12;

	if (udpfd < 0 || !d->ip)
		return;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = d->ip;
	to.sin_port = htons(137);
	sendto(udpfd, nbstat, sizeof(nbstat), 0, (struct sockaddr *)&to, sizeof(to));

	/* unicast mDNS reverse lookup straight to the device (RFC 6762 §5.5) */
	memset(q, 0, 12);
	q[5] = 1;  /* one question */
	snprintf(rev, sizeof(rev), "%u.%u.%u.%u.in-addr.arpa", b[3], b[2], b[1], b[0]);
	for (lab = strtok(rev, "."); lab; lab = strtok(NULL, ".")) {
		size_t l = strlen(lab);

		q[n++] = (uint8_t)l;
		memcpy(q + n, lab, l);
		n += l;
	}
	q[n++] = 0;
	q[n++] = 0; q[n++] = 12;  /* PTR */
	q[n++] = 0; q[n++] = 1;   /* IN */
	to.sin_port = htons(5353);
	sendto(udpfd, q, n, 0, (struct sockaddr *)&to, sizeof(to));
	d->name_tried = now_s();
}

/* Ask Cast devices to announce themselves (their TXT carries the real name). */
static void send_cast_query(void)
{
	static const uint8_t q[] = {
		0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
		11, '_', 'g', 'o', 'o', 'g', 'l', 'e', 'c', 'a', 's', 't', 4, '_', 't', 'c', 'p',
		5, 'l', 'o', 'c', 'a', 'l', 0, 0, 12, 0, 1,
	};
	struct sockaddr_in to;

	if (udpfd < 0)
		return;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(5353);
	inet_pton(AF_INET, "224.0.0.251", &to.sin_addr);
	if (my_ip)
		setsockopt(udpfd, IPPROTO_IP, IP_MULTICAST_IF, &my_ip, sizeof(my_ip));
	sendto(udpfd, q, sizeof(q), 0, (struct sockaddr *)&to, sizeof(to));
}

static struct uloop_timeout sweep_timer, step_timer;
static uint32_t sweep_next, sweep_end;   /* host order */

static void wifi_update(void)
{
#ifdef NO_IWINFO
	return;
#else
	const struct iwinfo_ops *iw;
	char buf[IWINFO_BUFSIZE];
	int len = 0, i;

	if (!cfg_wifi || !*cfg_wifi || !(iw = iwinfo_backend(cfg_wifi)))
		return;
	for (i = 0; i < (int)ndev; i++)
		devs[i].signal = 0;
	if (iw->assoclist(cfg_wifi, buf, &len) == 0)
		for (i = 0; i + (int)sizeof(struct iwinfo_assoclist_entry) <= len;
		     i += (int)sizeof(struct iwinfo_assoclist_entry)) {
			struct iwinfo_assoclist_entry *e = (struct iwinfo_assoclist_entry *)(buf + i);
			struct dev *d;

			if (!mac_valid(e->mac))
				continue;
			d = dev_seen(e->mac, V_WIFI);
			d->signal = e->signal ? e->signal : -1;
		}
	iwinfo_finish();
#endif
}

static void step_cb(struct uloop_timeout *t)
{
	unsigned i;
	uint32_t now = now_s();

	if (sweep_next <= sweep_end) {
		uint32_t ip = htonl(sweep_next++);

		if (ip_in_lan(ip))
			send_arp(ip);
		uloop_timeout_set(t, SWEEP_GAP_MS);
		return;
	}
	/* Sweep done: names for nameless devices that answered recently. */
	for (i = 0; i < ndev; i++)
		if (devs[i].ip && !devs[i].name[0] && now - devs[i].last_seen < 60 &&
		    now - devs[i].name_tried >= NAME_RETRY)
			send_name_queries(&devs[i]);
	send_cast_query();
	sweeps++;
	sweep_last = now;
}

/* Label the device that is our default route (the internet gateway). */
static void gateway_update(void)
{
	char line[256];
	FILE *f = fopen("/proc/net/route", "r");
	struct dev *d;

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char ifn[IFNAMSIZ + 1];
		unsigned long dst, gw;

		if (sscanf(line, "%16s %lx %lx", ifn, &dst, &gw) == 3 && dst == 0 && gw &&
		    (d = dev_by_ip((uint32_t)gw)))
			dev_set_info(d, "default gateway", I_GATEWAY);
	}
	fclose(f);
}

static void self_update(void)
{
	char host[NAME_LEN] = "";
	struct dev *d;
	FILE *f = fopen("/proc/sys/kernel/hostname", "r");

	if (f) {
		if (fgets(host, sizeof(host), f))
			host[strcspn(host, "\n")] = '\0';
		fclose(f);
	}
	if (!my_mac[0] && !my_mac[1] && !my_mac[2])
		return;
	d = dev_find(my_mac);
	if (!d) {
		d = dev_alloc();
		memcpy(d->mac, my_mac, 6);
		d->first_seen = now_s();
	}
	d->last_seen = now_s();
	d->via |= V_SELF;
	d->ip = my_ip;
	d->vendor = oui_lookup(my_mac);
	if (host[0])
		dev_set_name(d, host, N_SELF);
	dev_set_info(d, "this router (lanwatch)", I_SELF);
}

static void sweep_cb(struct uloop_timeout *t)
{
	uint32_t net, bcast, now = now_s();

	uloop_timeout_set(t, cfg_sweep * 1000);
	if (!if_info()) {
		syslog(LOG_WARNING, "%s has no IPv4 address yet", cfg_if);
		return;
	}
	if ((int)if_nametoindex(cfg_if) != ifindex)
		raw_open();
	self_update();
	wifi_update();
	gateway_update();
	net = ntohl(my_ip & my_mask);
	bcast = ntohl((my_ip & my_mask) | ~my_mask);
	if (bcast - net > 1024) {  /* big subnets: only the /24 around us */
		net = ntohl(my_ip) & 0xffffff00u;
		bcast = net | 0xff;
	}
	sweep_next = net + 1;
	sweep_end = bcast - 1;
	uloop_timeout_set(&step_timer, 1);
	if (cfg_state && ((unsaved_new && now - last_save >= SAVE_MIN_GAP) ||
	                  (dirty && now - last_save >= 86400)))
		state_save();
}

/* ------------------------------------------------------------------ */
/* ubus                                                               */
/* ------------------------------------------------------------------ */

static struct blob_buf b;

static bool dev_online(const struct dev *d, uint32_t now)
{
	return (d->via & V_SELF) || now - d->last_seen <= cfg_sweep * 2 + 60;
}

static int m_devices(struct ubus_context *ctx, struct ubus_object *obj,
                     struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	uint32_t now = now_s();
	void *arr;
	unsigned i, k;

	(void)obj; (void)method; (void)msg;
	blob_buf_init(&b, 0);
	arr = blobmsg_open_array(&b, "devices");
	for (i = 0; i < ndev; i++) {
		const struct dev *d = &devs[i];
		char mac[18], vendor[48], ip[16];
		void *t, *v;

		snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
		         d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
		t = blobmsg_open_table(&b, NULL);
		blobmsg_add_string(&b, "mac", mac);
		if (d->ip)
			blobmsg_add_string(&b, "ip", inet_ntop(AF_INET, &d->ip, ip, sizeof(ip)));
		if (d->name[0]) {
			blobmsg_add_string(&b, "name", d->name);
			blobmsg_add_string(&b, "name_source", name_src_names[d->name_src < 8 ? d->name_src : 0]);
		}
		oui_name(d->vendor, vendor, sizeof(vendor));
		if (vendor[0])
			blobmsg_add_string(&b, "vendor", vendor);
		blobmsg_add_u8(&b, "private_mac", (d->mac[0] & 2) != 0);
		if (d->info[0]) {
			blobmsg_add_string(&b, "info", d->info);
			blobmsg_add_string(&b, "info_source", info_src_names[d->info_src < 6 ? d->info_src : 0]);
		}
		blobmsg_add_u32(&b, "first_seen", d->first_seen);
		blobmsg_add_u32(&b, "last_seen", d->last_seen);
		blobmsg_add_u8(&b, "online", dev_online(d, now));
		blobmsg_add_u8(&b, "new", now - d->first_seen < NEW_SECONDS);
		if (d->signal)
			blobmsg_add_u32(&b, "wifi_signal", (uint32_t)(int32_t)d->signal);
		v = blobmsg_open_array(&b, "seen_via");
		for (k = 0; k < 7; k++)
			if (d->via & (1u << k))
				blobmsg_add_string(&b, NULL, via_names[k]);
		blobmsg_close_array(&b, v);
		blobmsg_close_table(&b, t);
	}
	blobmsg_close_array(&b, arr);
	return ubus_send_reply(ctx, req, b.head);
}

static int m_status(struct ubus_context *ctx, struct ubus_object *obj,
                    struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	uint32_t now = now_s();
	unsigned i, online = 0, fresh = 0;
	char ip[16];

	(void)obj; (void)method; (void)msg;
	for (i = 0; i < ndev; i++) {
		online += dev_online(&devs[i], now);
		fresh += now - devs[i].first_seen < NEW_SECONDS;
	}
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "interface", cfg_if);
	if (my_ip)
		blobmsg_add_string(&b, "address", inet_ntop(AF_INET, &my_ip, ip, sizeof(ip)));
	blobmsg_add_u32(&b, "devices", ndev);
	blobmsg_add_u32(&b, "online", online);
	blobmsg_add_u32(&b, "new", fresh);
	blobmsg_add_u32(&b, "sweep_interval", cfg_sweep);
	blobmsg_add_u32(&b, "sweeps", sweeps);
	blobmsg_add_u32(&b, "last_sweep", sweep_last);
	blobmsg_add_u32(&b, "frames_seen", frames_seen);
	blobmsg_add_u32(&b, "now", now);
	if (cfg_state) {
		blobmsg_add_string(&b, "state_file", cfg_state);
		blobmsg_add_u32(&b, "last_save", last_save);
	}
	return ubus_send_reply(ctx, req, b.head);
}

static const struct ubus_method methods[] = {
	UBUS_METHOD_NOARG("devices", m_devices),
	UBUS_METHOD_NOARG("status", m_status),
};
static struct ubus_object_type obj_type = UBUS_OBJECT_TYPE("lanwatch", methods);
static struct ubus_object ubus_obj = {
	.name = "lanwatch", .type = &obj_type, .methods = methods, .n_methods = ARRAY_SIZE(methods),
};
static struct ubus_auto_conn conn;
static bool ubus_ever_connected;

static void ubus_connected(struct ubus_context *ctx)
{
	int ret = ubus_add_object(ctx, &ubus_obj);

	ubus_ever_connected = true;
	if (ret)
		syslog(LOG_ERR, "ubus_add_object: %s", ubus_strerror(ret));
}

/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -i device    LAN bridge to watch (default br-lan)\n"
		"  -w device    Wi-Fi interface for signal of our own clients (default wlan0, '' = none)\n"
		"  -f file      keep the device list in file (written on new devices, daily, on stop)\n"
		"  -o file      vendor table (default /usr/share/lanwatch/oui.bin)\n"
		"  -t seconds   ARP sweep interval (default 300, minimum 60)\n"
		"  -s socket    ubus socket path\n", prog);
	exit(2);
}

int main(int argc, char **argv)
{
	int opt;
	struct sockaddr_in any;

	while ((opt = getopt(argc, argv, "i:w:f:o:t:s:h")) != -1) {
		switch (opt) {
		case 'i': cfg_if = optarg; break;
		case 'w': cfg_wifi = optarg; break;
		case 'f': cfg_state = *optarg ? optarg : NULL; break;
		case 'o': cfg_oui = optarg; break;
		case 't':
			cfg_sweep = (unsigned)strtoul(optarg, NULL, 10);
			if (cfg_sweep < 60 || cfg_sweep > 86400)
				usage(argv[0]);
			break;
		case 's': cfg_socket = optarg; break;
		default: usage(argv[0]);
		}
	}
	if (optind != argc)
		usage(argv[0]);

	openlog("lanwatch", LOG_PID | LOG_PERROR, LOG_DAEMON);
	signal(SIGPIPE, SIG_IGN);
	if (cfg_state) {  /* create the directory (e.g. /srv/lanwatch) if needed */
		char dir[PATH_MAX], *slash;

		snprintf(dir, sizeof(dir), "%s", cfg_state);
		if ((slash = strrchr(dir, '/')) && slash != dir) {
			*slash = '\0';
			mkdir(dir, 0755);
		}
	}
	oui_load();
	state_load();

	udpfd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	memset(&any, 0, sizeof(any));
	any.sin_family = AF_INET;
	if (udpfd >= 0 && bind(udpfd, (struct sockaddr *)&any, sizeof(any)) < 0) {
		close(udpfd);
		udpfd = -1;
	}

	if_info();  /* address and netmask first: the parsers check senders against them */
	uloop_init();
	if (udpfd >= 0) {
		udpu.fd = udpfd;
		udpu.cb = udp_cb;
		uloop_fd_add(&udpu, ULOOP_READ);
	}
	if (!raw_open())
		syslog(LOG_WARNING, "%s not available yet; will retry", cfg_if);
	conn.path = cfg_socket;
	conn.cb = ubus_connected;
	ubus_auto_connect(&conn);
	sweep_timer.cb = sweep_cb;
	step_timer.cb = step_cb;
	uloop_timeout_set(&sweep_timer, 2000);
	syslog(LOG_INFO, "watching %s, sweeping every %us", cfg_if, cfg_sweep);

	uloop_run();

	if (cfg_state && (dirty || unsaved_new))
		state_save();
	if (ubus_ever_connected)
		ubus_shutdown(&conn.ctx);
	uloop_done();
	return 0;
}
