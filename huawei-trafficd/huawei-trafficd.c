/*
 * huawei-trafficd: periodically sample the traffic counters of a Huawei
 * HiLink LTE router and expose the history on ubus as "huawei.traffic".
 *
 * Collection: GET /api/webserver/SesTokInfo (anonymous session cookie), then
 * GET /api/monitoring/traffic-statistics with that cookie and, unless -G is
 * given, /api/device/signal and /api/monitoring/status for signal quality
 * and connection state. No login needed.
 * Everything is non-blocking inside a single uloop, so ubus calls are never
 * held up by a slow or unreachable Huawei router.
 *
 * Optionally (-A/-B) also the powerline's PHY rate: once per sample, a HomePlug
 * AV CM_NW_STATS request to the powerline network's coordinator, on a raw
 * Ethernet socket.
 *
 * Only the router's existing shared libraries are used: libubus, libubox and
 * uClibc.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <libubus.h>
#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubox/usock.h>

#define HTTP_TIMEOUT_MS   5000
#define HTTP_BUF_SIZE     4096
#define COOKIE_SIZE       512
#define MIN_VALID_TIME    1577836800u /* 2020-01-01: clock not set before this */
#define QUERY_DEFAULT_MAX 1000
#define QUERY_HARD_MAX    4000        /* keeps replies well under ubus' 1 MiB */
#define STATE_MAGIC       0x48545246u /* "HTRF" */
#define STATE_VERSION     6u          /* v6: 80-byte samples with powerline rate; v1-v5 still load */
#define SIG_NONE          (-128)      /* unknown rsrq/sinr/rssi */
#define RSRP_NONE         INT16_MIN   /* unknown rsrp */
#define NET_NONE          255         /* unknown network type */
#define CONNECTED         901         /* Huawei ConnectionStatus: connected */

struct sample {
	uint32_t time;          /* Unix time on the OpenWrt router */
	uint32_t uptime;        /* Huawei CurrentConnectTime, seconds */
	uint64_t download;      /* CurrentDownload, bytes this session */
	uint64_t upload;        /* CurrentUpload, bytes this session */
	uint64_t total_download;
	uint64_t total_upload;
	/* Signal and connection state. Fine samples hold the reading taken with
	 * the counters; a coarse sample holds the average over its period (sig_n
	 * readings) and any non-connected status / other network type seen. */
	uint32_t cell_id;       /* 0 = unknown */
	uint16_t conn_status;   /* ConnectionStatus, 901 = connected; 0 = unknown */
	int16_t rsrp;           /* dBm */
	int8_t rsrq, sinr, rssi;/* dB, dB, dBm */
	uint8_t net_type;       /* CurrentNetworkType, 19 = LTE */
	uint8_t sig_n;          /* signal readings behind rsrp..rssi; 0 = none */
	/* Internet latency (ICMP echo to the targets) during the period. */
	uint8_t ping_ok;        /* bitmask of targets that answered at least once */
	uint16_t ping_sent;     /* probes resolved in this period; 0 = no data */
	uint16_t ping_lost;     /* ... of which got no reply */
	uint16_t rtt_min, rtt_avg, rtt_max;  /* ms, over the replies */
	/* Latency to the Huawei itself (its LAN address): only the local link,
	 * which here is a powerline. Probes alongside the internet ones. */
	uint16_t gw_sent;       /* probes resolved in this period; 0 = no data */
	uint16_t gw_lost;       /* ... of which got no reply */
	uint16_t gw_rtt_avg, gw_rtt_max;  /* ms, over the replies */
	/* Powerline PHY rate (HomePlug AV), as the coordinator reports it for
	 * the station on our side: average over plc_n readings. */
	uint16_t plc_down;      /* Mbit/s, coordinator -> our station (towards us) */
	uint16_t plc_up;        /* Mbit/s, our station -> coordinator */
	uint16_t plc_n;         /* readings behind them; 0 = none */
	uint16_t plc_reserved;
};

/* State files store samples as raw structs: the size must not change silently.
 * Version 5 files hold the first 72 bytes of this layout (no plc_* fields),
 * version 4 files the first 64 (no gw_* fields either). */
typedef char sample_is_80_bytes[sizeof(struct sample) == 80 ? 1 : -1];
#define SAMPLE_V5_SIZE 72
#define SAMPLE_V4_SIZE 64

/* The 56-byte sample of state file version 3 (signal, no latency). */
struct sample_v3 {
	uint32_t time, uptime;
	uint64_t download, upload, total_download, total_upload;
	uint32_t cell_id;
	uint16_t conn_status;
	int16_t rsrp;
	int8_t rsrq, sinr, rssi;
	uint8_t net_type, sig_n, reserved[3];
};

/* The 40-byte sample of state file versions 1 and 2. */
struct sample_v2 {
	uint32_t time, uptime;
	uint64_t download, upload, total_download, total_upload;
};

static void sample_clear_signal(struct sample *s)
{
	s->cell_id = 0;
	s->conn_status = 0;
	s->rsrp = RSRP_NONE;
	s->rsrq = s->sinr = s->rssi = SIG_NONE;
	s->net_type = NET_NONE;
	s->sig_n = 0;
	s->ping_ok = 0;
	s->ping_sent = s->ping_lost = 0;
	s->rtt_min = s->rtt_avg = s->rtt_max = 0;
	s->gw_sent = s->gw_lost = s->gw_rtt_avg = s->gw_rtt_max = 0;
	s->plc_down = s->plc_up = s->plc_n = s->plc_reserved = 0;
}

/* ---- configuration ---- */
static const char *cfg_host = "192.168.0.1";
static const char *cfg_port = "80";
static const char *cfg_socket;     /* ubus socket, NULL = default */
static bool cfg_signal = true;     /* also read device/signal + monitoring/status */
static bool cfg_latency = true;    /* ICMP echo to the latency targets */
static bool cfg_gw_ping = true;    /* ... and to the Huawei itself (-H, if an IPv4 address) */
#define PING_MAX_TARGETS 4
static const char *cfg_targets[PING_MAX_TARGETS] = { "1.1.1.1", "8.8.8.8" };
static unsigned cfg_ntargets = 2;
static unsigned cfg_save_every;    /* seconds, 0 = only on exit */
static const char *cfg_flash_dir;  /* hourly history log on flash; NULL = off */
static unsigned cfg_flash_every = 86400;
static const char *cfg_plc_iface = "br-lan";  /* powerline rate: interface to ask on */
static const char *cfg_plc_cco, *cfg_plc_sta;  /* coordinator and our station's MACs */

/*
 * Two history tiers, each a ring buffer (oldest samples dropped first):
 *  - fine:   every sample taken (default every 60 s, 4320 = 3 days)
 *  - coarse: the first sample of each coarse_interval bucket (default
 *            hourly, 8760 = 365 days). The counters are cumulative, so
 *            usage between any two kept samples is still exact.
 */
struct ring {
	const char *name;
	const char *state;       /* optional persistence file */
	unsigned interval;       /* seconds; 0 disables the coarse tier */
	unsigned cap, head, count;
	struct sample *buf;
};

static struct ring fine   = { .name = "fine",   .interval = 60,   .cap = 4320 };
static struct ring coarse = { .name = "coarse", .interval = 3600, .cap = 8760 };

/* ---- status ---- */
static uint32_t last_attempt, last_success;
static unsigned failures;  /* consecutive */
static char last_error[96];
static char signal_error[96];  /* last signal/status read problem, if any */

static void ring_push(struct ring *r, const struct sample *s)
{
	r->buf[(r->head + r->count) % r->cap] = *s;
	if (r->count < r->cap)
		r->count++;
	else
		r->head = (r->head + 1) % r->cap;
}

static const struct sample *ring_at(const struct ring *r, unsigned i) /* 0 = oldest */
{
	return &r->buf[(r->head + i) % r->cap];
}

static long div_round(long sum, unsigned n)  /* nearest, halves away from 0 */
{
	return sum >= 0 ? (sum + (long)n / 2) / (long)n : -((-sum + (long)n / 2) / (long)n);
}

/*
 * Close a coarse sample's period [c->time, end): replace its single signal
 * reading with the average of the fine samples in that period, and keep any
 * non-connected status or different network type seen during it.
 */
static void coarse_finalize(struct sample *c, uint32_t end)
{
	long rsrp = 0, rsrq = 0, sinr = 0, rssi = 0;
	uint8_t net = c->net_type;  /* the type at the start of the period */
	unsigned n = 0, i;
	uint32_t p_sent = 0, p_lost = 0, p_rtt_w = 0, p_replies = 0;
	uint16_t p_min = 0, p_max = 0;
	uint8_t p_ok = 0;
	uint32_t g_sent = 0, g_lost = 0, g_rtt_w = 0, g_replies = 0;
	uint16_t g_max = 0;
	uint32_t l_down = 0, l_up = 0, l_n = 0;

	for (i = fine.count; i-- > 0;) {
		const struct sample *f = ring_at(&fine, i);

		if (f->time < c->time)
			break;
		if (f->time >= end)
			continue;
		if (f->conn_status && f->conn_status != CONNECTED)
			c->conn_status = f->conn_status;
		if (f->net_type != NET_NONE && f->net_type != net)
			c->net_type = f->net_type;  /* any other type seen sticks */
		if (f->ping_sent) {
			unsigned r = f->ping_sent > f->ping_lost ? f->ping_sent - f->ping_lost : 0;

			p_sent += f->ping_sent;
			p_lost += f->ping_lost;
			p_ok |= f->ping_ok;
			if (r) {
				p_rtt_w += (uint32_t)f->rtt_avg * r;
				p_replies += r;
				if (!p_min || f->rtt_min < p_min)
					p_min = f->rtt_min;
				if (f->rtt_max > p_max)
					p_max = f->rtt_max;
			}
		}
		if (f->gw_sent) {
			unsigned r = f->gw_sent > f->gw_lost ? f->gw_sent - f->gw_lost : 0;

			g_sent += f->gw_sent;
			g_lost += f->gw_lost;
			if (r) {
				g_rtt_w += (uint32_t)f->gw_rtt_avg * r;
				g_replies += r;
				if (f->gw_rtt_max > g_max)
					g_max = f->gw_rtt_max;
			}
		}
		if (f->plc_n) {  /* reading-weighted, like a plain average of all readings */
			l_down += (uint32_t)f->plc_down * f->plc_n;
			l_up += (uint32_t)f->plc_up * f->plc_n;
			l_n += f->plc_n;
		}
		if (!f->sig_n || f->rsrp == RSRP_NONE)
			continue;
		rsrp += f->rsrp;
		rsrq += f->rsrq;
		sinr += f->sinr;
		rssi += f->rssi;
		n++;
	}
	if (p_sent) {  /* latency over the period: totals, weighted average, extremes */
		c->ping_sent = p_sent > 65535 ? 65535 : (uint16_t)p_sent;
		c->ping_lost = p_lost > 65535 ? 65535 : (uint16_t)p_lost;
		c->ping_ok = p_ok;
		c->rtt_avg = p_replies ? (uint16_t)((p_rtt_w + p_replies / 2) / p_replies) : 0;
		c->rtt_min = p_min;
		c->rtt_max = p_max;
	}
	if (g_sent) {
		c->gw_sent = g_sent > 65535 ? 65535 : (uint16_t)g_sent;
		c->gw_lost = g_lost > 65535 ? 65535 : (uint16_t)g_lost;
		c->gw_rtt_avg = g_replies ? (uint16_t)((g_rtt_w + g_replies / 2) / g_replies) : 0;
		c->gw_rtt_max = g_max;
	}
	if (l_n) {
		c->plc_down = (uint16_t)((l_down + l_n / 2) / l_n);
		c->plc_up = (uint16_t)((l_up + l_n / 2) / l_n);
		c->plc_n = l_n > 65535 ? 65535 : (uint16_t)l_n;
	}
	if (!n)
		return;
	c->rsrp = (int16_t)div_round(rsrp, n);
	c->rsrq = (int8_t)div_round(rsrq, n);
	c->sinr = (int8_t)div_round(sinr, n);
	c->rssi = (int8_t)div_round(rssi, n);
	c->sig_n = n > 255 ? 255 : (uint8_t)n;
}

/* Keep s in the coarse tier if it starts a new coarse_interval bucket. */
static void coarse_offer(const struct sample *s)
{
	struct sample *last;

	if (!coarse.interval)
		return;
	if (coarse.count) {
		last = &coarse.buf[(coarse.head + coarse.count - 1) % coarse.cap];
		if (last->time / coarse.interval == s->time / coarse.interval)
			return;
		coarse_finalize(last, s->time);
	}
	ring_push(&coarse, s);
}

static void record_sample(const struct sample *s)
{
	ring_push(&fine, s);
	coarse_offer(s);
}

/* ------------------------------------------------------------------ */
/* Persistence (optional)                                             */
/* ------------------------------------------------------------------ */

static void ring_save(const struct ring *r)
{
	char tmp[PATH_MAX];
	uint32_t hdr[4] = { STATE_MAGIC, STATE_VERSION, r->count, r->interval };
	unsigned i;
	FILE *f;

	if (!r->state)
		return;
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", r->state) >= (int)sizeof(tmp))
		return;
	f = fopen(tmp, "wb");
	if (!f) {
		syslog(LOG_WARNING, "cannot write %s: %s", tmp, strerror(errno));
		return;
	}
	fwrite(hdr, sizeof(hdr), 1, f);
	for (i = 0; i < r->count; i++)
		fwrite(ring_at(r, i), sizeof(struct sample), 1, f);
	if (fflush(f) != 0 || ferror(f) || fsync(fileno(f)) != 0) {
		fclose(f);
		unlink(tmp);
		syslog(LOG_WARNING, "write to %s failed", tmp);
		return;
	}
	fclose(f);
	if (rename(tmp, r->state) != 0) {
		unlink(tmp);
		syslog(LOG_WARNING, "rename to %s failed: %s", r->state, strerror(errno));
	}
}

static void state_save(void)
{
	ring_save(&fine);
	if (coarse.interval)
		ring_save(&coarse);
}

static void ring_load(struct ring *r)
{
	uint32_t hdr[4];
	struct sample s;
	unsigned i;
	FILE *f;

	if (!r->state || !(f = fopen(r->state, "rb")))
		return;
	/* v1: magic, version, count. v2 adds the tier interval. */
	if (fread(hdr, sizeof(uint32_t), 3, f) != 3 || hdr[0] != STATE_MAGIC ||
	    hdr[1] < 1 || hdr[1] > STATE_VERSION ||
	    (hdr[1] >= 2 && fread(&hdr[3], sizeof(uint32_t), 1, f) != 1)) {
		syslog(LOG_WARNING, "ignoring unrecognised state file %s", r->state);
		fclose(f);
		return;
	}
	if (hdr[1] >= 2 && hdr[3] != r->interval)
		syslog(LOG_NOTICE, "%s was recorded at %us intervals, now %us",
		       r->state, hdr[3], r->interval);
	for (i = 0; i < hdr[2]; i++) {
		if (hdr[1] >= 6) {
			if (fread(&s, sizeof(s), 1, f) != 1)
				break;
		} else if (hdr[1] == 5) {  /* 72-byte samples: no powerline rate */
			if (fread(&s, SAMPLE_V5_SIZE, 1, f) != 1)
				break;
			s.plc_down = s.plc_up = s.plc_n = s.plc_reserved = 0;
		} else if (hdr[1] == 4) {  /* 64-byte samples: no Huawei latency */
			if (fread(&s, SAMPLE_V4_SIZE, 1, f) != 1)
				break;
			s.gw_sent = s.gw_lost = s.gw_rtt_avg = s.gw_rtt_max = 0;
			s.plc_down = s.plc_up = s.plc_n = s.plc_reserved = 0;
		} else if (hdr[1] == 3) {  /* 56-byte samples: signal, no latency */
			struct sample_v3 o;

			if (fread(&o, sizeof(o), 1, f) != 1)
				break;
			sample_clear_signal(&s);
			s.time = o.time;
			s.uptime = o.uptime;
			s.download = o.download;
			s.upload = o.upload;
			s.total_download = o.total_download;
			s.total_upload = o.total_upload;
			s.cell_id = o.cell_id;
			s.conn_status = o.conn_status;
			s.rsrp = o.rsrp;
			s.rsrq = o.rsrq;
			s.sinr = o.sinr;
			s.rssi = o.rssi;
			s.net_type = o.net_type;
			s.sig_n = o.sig_n;
		} else {  /* older 40-byte samples: no signal data */
			struct sample_v2 o;

			if (fread(&o, sizeof(o), 1, f) != 1)
				break;
			s.time = o.time;
			s.uptime = o.uptime;
			s.download = o.download;
			s.upload = o.upload;
			s.total_download = o.total_download;
			s.total_upload = o.total_upload;
			sample_clear_signal(&s);
		}
		ring_push(r, &s);  /* keeps only the newest r->cap */
	}
	syslog(LOG_INFO, "loaded %u %s samples from %s", r->count, r->name, r->state);
	fclose(f);
}

/* ------------------------------------------------------------------ */
/* Hourly history on flash (optional): compressed, append-only        */
/* ------------------------------------------------------------------ */
/*
 * Finished coarse samples are appended to one file per month (UTC),
 * <dir>/YYYY-MM.log, once every flash_every seconds and on clean stop.
 * Whole old months are deleted, so files are never rewritten.
 *
 * Each append is one block: 12-byte header + delta-encoded records.
 *   "HB", version (1; 2 when records may carry F_GW; 3 F_PLC), record count,
 *   payload length (u16 LE), 0 (u16),
 *   CRC-32 of the payload (u32 LE)
 * The first record of a block is encoded against an all-zero sample, so
 * every block decodes on its own and a torn write loses only that block.
 * Record: flags, then varints: time delta, uptime drift (uptime delta minus
 * time delta), session download/upload deltas, total-minus-session offset
 * deltas; [signal: rsrp+140, rsrq+40, sinr+40, rssi+140, readings];
 * [meta: cell, status, network type]; [ping: sent, lost, rtt min/avg/max,
 * ok mask]; [gw: sent, lost, rtt avg/max]; [plc: down, up, readings].
 * About 35 bytes per hourly record.
 */
#define FLASH_MONTHS     13           /* keep this many monthly files */
#define FLASH_MIN_FREE   (64 * 1024)  /* never fill the overlay past this */
#define BLOCK_HDR        12
#define BLOCK_MAX        255          /* records per block */
#define REC_MAX          112          /* worst case: 1 + 6x10 + 5 + 9 + 16 + 12 + 9 = 112 bytes */
#define F_SIG            1
#define F_META           2
#define F_PING           4
#define F_GW             8            /* block version 2 */
#define F_PLC            16           /* block version 3 */
#define BLOCK_VERSION    3            /* written; 1 and 2 are still read */
#define TMPFS_MAGIC_     0x01021994
#define RAMFS_MAGIC_     0x858458f6

static uint32_t flushed_until;        /* newest coarse sample time on flash */
static uint32_t flash_last_write;
static char flash_error[96];

static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
	uint32_t c = 0xffffffffu;
	int k;

	while (n--) {
		c ^= *p++;
		for (k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xedb88320u & -(c & 1));
	}
	return ~c;
}

static size_t put_uv(uint8_t *p, uint64_t v)
{
	size_t n = 0;

	do {
		uint8_t b = v & 0x7f;

		v >>= 7;
		p[n++] = b | (v ? 0x80 : 0);
	} while (v);
	return n;
}

static size_t put_sv(uint8_t *p, int64_t v)
{
	return put_uv(p, ((uint64_t)v << 1) ^ (uint64_t)(v >> 63));
}

static bool get_uv(const uint8_t **p, const uint8_t *end, uint64_t *v)
{
	uint64_t r = 0;
	int sh;

	for (sh = 0; *p < end && sh < 64; sh += 7) {
		uint8_t b = *(*p)++;

		r |= (uint64_t)(b & 0x7f) << sh;
		if (!(b & 0x80)) {
			*v = r;
			return true;
		}
	}
	return false;
}

static bool get_sv(const uint8_t **p, const uint8_t *end, int64_t *v)
{
	uint64_t u;

	if (!get_uv(p, end, &u))
		return false;
	*v = (int64_t)(u >> 1) ^ -(int64_t)(u & 1);
	return true;
}

static uint8_t bias8(int v, int bias)  /* small signed value -> 0..255 */
{
	v += bias;
	return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

static void zero_sample(struct sample *z)
{
	memset(z, 0, sizeof(*z));
	sample_clear_signal(z);
}

static int64_t offset(uint64_t total, uint64_t session)
{
	return (int64_t)(total - session);
}

static size_t rec_encode(uint8_t *p, const struct sample *a, const struct sample *b)
{
	bool sig = b->sig_n && b->rsrp != RSRP_NONE;
	bool meta = a->cell_id != b->cell_id || a->conn_status != b->conn_status ||
	            a->net_type != b->net_type;
	int64_t dt = (int64_t)b->time - a->time;
	size_t n = 0;

	p[n++] = (sig ? F_SIG : 0) | (meta ? F_META : 0) | (b->ping_sent ? F_PING : 0) |
	         (b->gw_sent ? F_GW : 0) | (b->plc_n ? F_PLC : 0);
	n += put_sv(p + n, dt);
	n += put_sv(p + n, (int64_t)b->uptime - a->uptime - dt);
	n += put_sv(p + n, (int64_t)(b->download - a->download));
	n += put_sv(p + n, (int64_t)(b->upload - a->upload));
	n += put_sv(p + n, offset(b->total_download, b->download) - offset(a->total_download, a->download));
	n += put_sv(p + n, offset(b->total_upload, b->upload) - offset(a->total_upload, a->upload));
	if (sig) {
		p[n++] = bias8(b->rsrp, 140);
		p[n++] = bias8(b->rsrq, 40);
		p[n++] = bias8(b->sinr, 40);
		p[n++] = b->rssi == SIG_NONE ? 0 : bias8(b->rssi, 140);
		p[n++] = b->sig_n;
	}
	if (meta) {
		n += put_uv(p + n, b->cell_id);
		n += put_uv(p + n, b->conn_status);
		p[n++] = b->net_type;
	}
	if (b->ping_sent) {
		n += put_uv(p + n, b->ping_sent);
		n += put_uv(p + n, b->ping_lost);
		n += put_uv(p + n, b->rtt_min);
		n += put_uv(p + n, b->rtt_avg);
		n += put_uv(p + n, b->rtt_max);
		p[n++] = b->ping_ok;
	}
	if (b->gw_sent) {
		n += put_uv(p + n, b->gw_sent);
		n += put_uv(p + n, b->gw_lost);
		n += put_uv(p + n, b->gw_rtt_avg);
		n += put_uv(p + n, b->gw_rtt_max);
	}
	if (b->plc_n) {
		n += put_uv(p + n, b->plc_down);
		n += put_uv(p + n, b->plc_up);
		n += put_uv(p + n, b->plc_n);
	}
	return n;
}

static bool rec_decode(const uint8_t **p, const uint8_t *end,
                       const struct sample *a, struct sample *b)
{
	int64_t dt, du, dd, dup, od, ou;
	uint64_t cell, conn;
	uint8_t f;

	if (*p >= end)
		return false;
	f = *(*p)++;
	if (!get_sv(p, end, &dt) || !get_sv(p, end, &du) || !get_sv(p, end, &dd) ||
	    !get_sv(p, end, &dup) || !get_sv(p, end, &od) || !get_sv(p, end, &ou))
		return false;
	*b = *a;
	b->time = (uint32_t)((int64_t)a->time + dt);
	b->uptime = (uint32_t)((int64_t)a->uptime + dt + du);
	b->download = a->download + (uint64_t)dd;
	b->upload = a->upload + (uint64_t)dup;
	b->total_download = b->download + (uint64_t)(offset(a->total_download, a->download) + od);
	b->total_upload = b->upload + (uint64_t)(offset(a->total_upload, a->upload) + ou);
	b->rsrp = RSRP_NONE;
	b->rsrq = b->sinr = b->rssi = SIG_NONE;
	b->sig_n = 0;
	b->ping_ok = 0;
	b->ping_sent = b->ping_lost = b->rtt_min = b->rtt_avg = b->rtt_max = 0;
	b->gw_sent = b->gw_lost = b->gw_rtt_avg = b->gw_rtt_max = 0;
	b->plc_down = b->plc_up = b->plc_n = b->plc_reserved = 0;
	if (f & F_SIG) {
		if (end - *p < 5)
			return false;
		b->rsrp = (int16_t)((*p)[0] - 140);
		b->rsrq = (int8_t)((*p)[1] - 40);
		b->sinr = (int8_t)((*p)[2] - 40);
		b->rssi = (*p)[3] ? (int8_t)((*p)[3] - 140) : SIG_NONE;
		b->sig_n = (*p)[4];
		*p += 5;
	}
	if (f & F_META) {
		if (!get_uv(p, end, &cell) || !get_uv(p, end, &conn) || *p >= end)
			return false;
		b->cell_id = (uint32_t)cell;
		b->conn_status = (uint16_t)conn;
		b->net_type = *(*p)++;
	}
	if (f & F_PING) {
		uint64_t v[5];
		int k;

		for (k = 0; k < 5; k++)
			if (!get_uv(p, end, &v[k]) || v[k] > 65535)
				return false;
		if (*p >= end)
			return false;
		b->ping_sent = (uint16_t)v[0];
		b->ping_lost = (uint16_t)v[1];
		b->rtt_min = (uint16_t)v[2];
		b->rtt_avg = (uint16_t)v[3];
		b->rtt_max = (uint16_t)v[4];
		b->ping_ok = *(*p)++;
	}
	if (f & F_GW) {
		uint64_t v[4];
		int k;

		for (k = 0; k < 4; k++)
			if (!get_uv(p, end, &v[k]) || v[k] > 65535)
				return false;
		b->gw_sent = (uint16_t)v[0];
		b->gw_lost = (uint16_t)v[1];
		b->gw_rtt_avg = (uint16_t)v[2];
		b->gw_rtt_max = (uint16_t)v[3];
	}
	if (f & F_PLC) {
		uint64_t v[3];
		int k;

		for (k = 0; k < 3; k++)
			if (!get_uv(p, end, &v[k]) || v[k] > 65535)
				return false;
		b->plc_down = (uint16_t)v[0];
		b->plc_up = (uint16_t)v[1];
		b->plc_n = (uint16_t)v[2];
	}
	return true;
}

static unsigned month_of(uint32_t t)  /* year * 12 + month, UTC */
{
	time_t tt = t;
	struct tm tm;

	gmtime_r(&tt, &tm);
	return (unsigned)(tm.tm_year + 1900) * 12 + (unsigned)tm.tm_mon;
}

static bool month_file(char *buf, size_t size, unsigned month)
{
	return snprintf(buf, size, "%s/%04u-%02u.log", cfg_flash_dir, month / 12, month % 12 + 1) < (int)size;
}

/* Parse "YYYY-MM.log"; returns 0 if the name doesn't match. */
static unsigned month_from_name(const char *name)
{
	unsigned y, m;
	char tail[8];

	if (sscanf(name, "%4u-%2u%7s", &y, &m, tail) != 3 || strcmp(tail, ".log") ||
	    strlen(name) != 11 || y < 2000 || m < 1 || m > 12)
		return 0;
	return y * 12 + m - 1;
}

/* Months present on flash, sorted ascending. Returns count. */
static unsigned flash_months(unsigned *out, unsigned max)
{
	DIR *d = opendir(cfg_flash_dir);
	struct dirent *e;
	unsigned n = 0, i, j, m;

	if (!d)
		return 0;
	while ((e = readdir(d)) && n < max)
		if ((m = month_from_name(e->d_name)))
			out[n++] = m;
	closedir(d);
	for (i = 1; i < n; i++)  /* insertion sort: a handful of entries */
		for (j = i; j > 0 && out[j - 1] > out[j]; j--) {
			m = out[j];
			out[j] = out[j - 1];
			out[j - 1] = m;
		}
	return n;
}

/*
 * Read one month's blocks, calling fn for each record. A damaged or torn
 * block ends the file: the file is cut back to the last good block so later
 * appends stay readable.
 */
static void flash_read_month(unsigned month, void (*fn)(const struct sample *))
{
	char path[PATH_MAX];
	uint8_t hdr[BLOCK_HDR], *buf = NULL;
	long good = 0;
	FILE *f;

	if (!month_file(path, sizeof(path), month) || !(f = fopen(path, "rb")))
		return;
	for (;;) {
		struct sample prev, cur;
		const uint8_t *p, *end;
		unsigned len, count, i;

		if (fread(hdr, 1, BLOCK_HDR, f) != BLOCK_HDR)
			break;
		len = hdr[4] | hdr[5] << 8;
		count = hdr[3];
		if (hdr[0] != 'H' || hdr[1] != 'B' || hdr[2] < 1 || hdr[2] > BLOCK_VERSION || !count)
			break;
		free(buf);
		if (!(buf = malloc(len ? len : 1)) || fread(buf, 1, len, f) != len)
			break;  /* torn tail */
		if (crc32_buf(buf, len) != ((uint32_t)hdr[8] | (uint32_t)hdr[9] << 8 |
		                           (uint32_t)hdr[10] << 16 | (uint32_t)hdr[11] << 24)) {
			/* Damaged inside, but its length is intact: skip just this block. */
			syslog(LOG_WARNING, "%s: skipping a damaged block at byte %ld", path, good);
			good = ftell(f);
			continue;
		}
		zero_sample(&prev);
		p = buf;
		end = buf + len;
		for (i = 0; i < count && rec_decode(&p, end, &prev, &cur); i++) {
			fn(&cur);
			prev = cur;
		}
		if (i < count)
			break;
		good = ftell(f);
	}
	free(buf);
	if (!feof(f) || ftell(f) != good) {
		fseek(f, 0, SEEK_END);
		if (ftell(f) != good) {
			syslog(LOG_WARNING, "%s: damaged data after byte %ld, truncating", path, good);
			if (truncate(path, good) != 0)
				syslog(LOG_WARNING, "truncate %s: %s", path, strerror(errno));
		}
	}
	fclose(f);
}

/* Refuse RAM-backed directories, which would silently not persist. */
static bool flash_dir_ok(void)
{
	char parent[PATH_MAX], *slash;
	struct statfs sf;

	snprintf(parent, sizeof(parent), "%s", cfg_flash_dir);
	if ((slash = strrchr(parent, '/')) && slash != parent) {
		*slash = '\0';
		mkdir(parent, 0755);  /* e.g. /srv */
	}
	if (mkdir(cfg_flash_dir, 0755) != 0 && errno != EEXIST) {
		snprintf(flash_error, sizeof(flash_error), "mkdir: %s", strerror(errno));
		return false;
	}
	if (statfs(cfg_flash_dir, &sf) == 0 &&
	    ((unsigned long)sf.f_type == TMPFS_MAGIC_ || (unsigned long)sf.f_type == RAMFS_MAGIC_)) {
		snprintf(flash_error, sizeof(flash_error), "%s is in RAM, not flash; disabled", cfg_flash_dir);
		return false;
	}
	return true;
}

static void flash_prune(unsigned newest)
{
	unsigned months[64], n = flash_months(months, 64), i;
	char path[PATH_MAX];

	for (i = 0; i < n; i++)
		if (months[i] + FLASH_MONTHS <= newest && month_file(path, sizeof(path), months[i])) {
			if (unlink(path) == 0)
				syslog(LOG_INFO, "removed old history %s", path);
		}
}

static bool flash_append(unsigned month, const uint8_t *payload, size_t len, unsigned count)
{
	char path[PATH_MAX];
	uint8_t hdr[BLOCK_HDR];
	uint32_t crc = crc32_buf(payload, len);
	uint8_t *buf;
	ssize_t w;
	int fd;

	hdr[0] = 'H'; hdr[1] = 'B'; hdr[2] = BLOCK_VERSION; hdr[3] = (uint8_t)count;
	hdr[4] = len & 0xff; hdr[5] = len >> 8; hdr[6] = hdr[7] = 0;
	hdr[8] = crc & 0xff; hdr[9] = crc >> 8 & 0xff; hdr[10] = crc >> 16 & 0xff; hdr[11] = crc >> 24;
	if (!month_file(path, sizeof(path), month))
		return false;
	/* One write() per block: JFFS2 then stores it as one atomic node. */
	if (!(buf = malloc(BLOCK_HDR + len)))
		return false;
	memcpy(buf, hdr, BLOCK_HDR);
	memcpy(buf + BLOCK_HDR, payload, len);
	fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
	if (fd < 0) {
		snprintf(flash_error, sizeof(flash_error), "open %.60s: %s", path, strerror(errno));
		free(buf);
		return false;
	}
	w = write(fd, buf, BLOCK_HDR + len);
	free(buf);
	if (w != (ssize_t)(BLOCK_HDR + len) || fsync(fd) != 0) {
		snprintf(flash_error, sizeof(flash_error), "write %.60s: %s", path, strerror(errno));
		close(fd);
		return false;
	}
	close(fd);
	return true;
}

/* Append finished coarse samples (all but the newest, still-open one). */
static void flash_flush(void)
{
	static uint8_t payload[BLOCK_MAX * REC_MAX];
	struct statvfs sv;
	struct sample prev;
	unsigned i, first, count = 0, month = 0, newest = 0;
	size_t len = 0;

	if (!cfg_flash_dir || !coarse.interval || coarse.count < 2)
		return;
	for (first = coarse.count - 1; first > 0 && ring_at(&coarse, first - 1)->time > flushed_until; first--)
		;
	if (first >= coarse.count - 1)
		return;  /* nothing new */
	if (statvfs(cfg_flash_dir, &sv) == 0 &&
	    (uint64_t)sv.f_bavail * sv.f_bsize < FLASH_MIN_FREE) {
		snprintf(flash_error, sizeof(flash_error), "less than %u KiB free on flash; not writing",
		         FLASH_MIN_FREE / 1024);
		syslog(LOG_WARNING, "%s", flash_error);
		return;
	}
	for (i = first; i < coarse.count - 1; i++) {
		const struct sample *s = ring_at(&coarse, i);
		unsigned m = month_of(s->time);

		if (s->time < MIN_VALID_TIME)
			continue;
		if (count && (m != month || count == BLOCK_MAX)) {
			if (!flash_append(month, payload, len, count))
				return;
			count = 0;
			len = 0;
		}
		if (!count)
			zero_sample(&prev);
		month = m;
		len += rec_encode(payload + len, &prev, s);
		prev = *s;
		count++;
		flushed_until = s->time;
		newest = m;
	}
	if (count && !flash_append(month, payload, len, count))
		return;
	flash_last_write = (uint32_t)time(NULL);
	flash_error[0] = '\0';
	if (newest)
		flash_prune(newest);
}

static struct uloop_timeout flash_timer;

static void flash_timer_cb(struct uloop_timeout *t)
{
	flash_flush();
	uloop_timeout_set(t, cfg_flash_every * 1000);
}

static struct sample *load_buf;
static unsigned load_n, load_cap;

static void collect_record(const struct sample *s)
{
	if (load_n == load_cap) {
		unsigned cap = load_cap ? load_cap * 2 : 1024;
		struct sample *nb = realloc(load_buf, cap * sizeof(*nb));

		if (!nb)
			return;
		load_buf = nb;
		load_cap = cap;
	}
	load_buf[load_n++] = *s;
}

/*
 * History on flash is older than (or the same as) what /tmp holds after a
 * service restart, so: flash records older than the oldest /tmp coarse
 * sample first, then the /tmp ones. After a reboot /tmp is empty and the
 * flash records are the whole hourly history.
 */
static void flash_load(void)
{
	unsigned months[64], n, i;
	uint32_t tmp_oldest = coarse.count ? ring_at(&coarse, 0)->time : UINT32_MAX;
	struct sample *tmp = NULL;
	unsigned tmp_n = coarse.count, loaded = 0;

	n = flash_months(months, 64);
	load_n = 0;
	for (i = 0; i < n; i++)
		flash_read_month(months[i], collect_record);
	if (load_n)
		flushed_until = load_buf[load_n - 1].time;
	if (tmp_n) {
		if (!(tmp = malloc(tmp_n * sizeof(*tmp))))
			goto out;
		for (i = 0; i < tmp_n; i++)
			tmp[i] = *ring_at(&coarse, i);
	}
	coarse.head = coarse.count = 0;
	for (i = 0; i < load_n; i++)
		if (load_buf[i].time < tmp_oldest) {
			ring_push(&coarse, &load_buf[i]);
			loaded++;
		}
	for (i = 0; i < tmp_n; i++)
		ring_push(&coarse, &tmp[i]);
	syslog(LOG_INFO, "loaded %u hourly samples from %s (%u files)", loaded, cfg_flash_dir, n);
out:
	free(tmp);
	free(load_buf);
	load_buf = NULL;
	load_n = load_cap = 0;
}

static void state_load(void)
{
	unsigned i;

	ring_load(&fine);
	if (!coarse.interval)
		return;
	ring_load(&coarse);
	if (cfg_flash_dir)
		flash_load();
	/* No coarse history yet (first run, or a v1 file): derive it. */
	if (!coarse.count)
		for (i = 0; i < fine.count; i++)
			coarse_offer(ring_at(&fine, i));
}

static struct uloop_timeout save_timer;

static void save_timer_cb(struct uloop_timeout *t)
{
	state_save();
	uloop_timeout_set(t, cfg_save_every * 1000);
}

/* ------------------------------------------------------------------ */
/* Non-blocking HTTP client for the Huawei API                        */
/* ------------------------------------------------------------------ */

enum stage { STAGE_TOKEN, STAGE_STATS, STAGE_SIGNAL, STAGE_STATUS };

static struct {
	struct uloop_fd fd;
	struct uloop_timeout timeout;
	bool busy, connecting, cookie_was_cached;
	enum stage stage;
	char req[COOKIE_SIZE + 256];
	size_t req_len, sent;
	char buf[HTTP_BUF_SIZE];
	size_t len;
	bool have_pending;       /* counters read; signal/status may follow */
	struct sample pending;
} http;

static char cookie[COOKIE_SIZE]; /* "SessionID=..." */

static void http_start(void);

#define FAIL(...) do { sample_failed(__VA_ARGS__); return; } while (0)

static void http_close(void)
{
	uloop_timeout_cancel(&http.timeout);
	if (http.fd.fd >= 0) {
		uloop_fd_delete(&http.fd);
		close(http.fd.fd);
		http.fd.fd = -1;
	}
}

static void finish_sample(void);
static void ping_attach(struct sample *s);
static void plc_attach(struct sample *s);

static void sample_failed(const char *fmt, ...)
{
	va_list ap;

	http_close();
	if (http.have_pending) {
		/* Counters were read; only signal/status failed. Keep the sample. */
		va_start(ap, fmt);
		vsnprintf(signal_error, sizeof(signal_error), fmt, ap);
		va_end(ap);
		finish_sample();
		return;
	}
	http.busy = false;
	va_start(ap, fmt);
	vsnprintf(last_error, sizeof(last_error), fmt, ap);
	va_end(ap);
	if (failures++ == 0)  /* log state changes, not every failure */
		syslog(LOG_WARNING, "sampling failed: %s", last_error);
}

/* Copy the text between <tag> and </tag> into out. */
static bool xml_text(const char *body, const char *tag, char *out, size_t size)
{
	char open[48], close_[48];
	const char *a, *b;

	snprintf(open, sizeof(open), "<%s>", tag);
	snprintf(close_, sizeof(close_), "</%s>", tag);
	if (!(a = strstr(body, open)))
		return false;
	a += strlen(open);
	if (!(b = strstr(a, close_)) || (size_t)(b - a) >= size)
		return false;
	memcpy(out, a, b - a);
	out[b - a] = '\0';
	return true;
}

static bool xml_u64(const char *body, const char *tag, uint64_t *v)
{
	char text[32], *end;

	if (!xml_text(body, tag, text, sizeof(text)) || !text[0])
		return false;
	errno = 0;
	*v = strtoull(text, &end, 10);
	return !errno && *end == '\0';
}

/* Integer from text like "-96dBm", ">=-51dBm" or "8dB". */
static bool xml_int(const char *body, const char *tag, long *v)
{
	char text[32], *p, *end;

	if (!xml_text(body, tag, text, sizeof(text)))
		return false;
	p = text + strcspn(text, "-0123456789");
	if (!*p)
		return false;
	*v = strtol(p, &end, 10);
	return end != p && (*p != '-' || end != p + 1);
}

static int8_t clamp8(long v)
{
	return v < -127 ? -127 : v > 127 ? 127 : (int8_t)v;  /* -128 means unknown */
}

static void finish_sample(void)
{
	http_close();
	http.busy = false;
	http.have_pending = false;
	ping_attach(&http.pending);
	plc_attach(&http.pending);
	record_sample(&http.pending);
	last_success = http.pending.time;
	if (failures)
		syslog(LOG_NOTICE, "sampling recovered after %u failures", failures);
	failures = 0;
	last_error[0] = '\0';
}

/* Returns pointer to body, or NULL if headers are incomplete. */
static char *http_body(size_t *content_length, bool *have_length)
{
	char *end = strstr(http.buf, "\r\n\r\n"), *p;

	if (!end)
		return NULL;
	*have_length = false;
	*end = '\0';
	p = strcasestr(http.buf, "\r\nContent-Length:");
	if (p) {
		*content_length = strtoul(p + 17, NULL, 10);
		*have_length = true;
	}
	*end = '\r';
	return end + 4;
}

static void handle_response(void)
{
	struct sample *s = &http.pending;
	char *body, code[16];
	size_t cl;
	bool have_cl;
	uint64_t v;
	long l, a, b2, c, d;

	if (strncmp(http.buf, "HTTP/1.", 7) || strncmp(http.buf + 8, " 200", 4))
		FAIL("HTTP status: %.12s", http.buf);
	if (!(body = http_body(&cl, &have_cl)))
		FAIL("malformed HTTP response");

	if (http.stage == STAGE_TOKEN) {
		if (!xml_text(body, "SesInfo", cookie, sizeof(cookie)) || !cookie[0]) {
			cookie[0] = '\0';
			FAIL("no SesInfo in token response");
		}
		http.stage = STAGE_STATS;
		http.cookie_was_cached = false;
		http_start();
		return;
	}

	if (http.stage == STAGE_SIGNAL || http.stage == STAGE_STATUS) {
		if (xml_text(body, "code", code, sizeof(code)))
			FAIL("Huawei API error %s", code);  /* keeps the sample */
		if (http.stage == STAGE_SIGNAL) {
			if (xml_int(body, "rsrp", &a) && xml_int(body, "rsrq", &b2) &&
			    xml_int(body, "sinr", &c)) {
				s->rsrp = a < -32767 ? -32767 : a > 32767 ? 32767 : (int16_t)a;
				s->rsrq = clamp8(b2);
				s->sinr = clamp8(c);
				s->rssi = xml_int(body, "rssi", &d) ? clamp8(d) : SIG_NONE;
				s->sig_n = 1;
			}
			if (xml_int(body, "cell_id", &l) && l > 0)
				s->cell_id = (uint32_t)l;
			http.stage = STAGE_STATUS;
			http_start();
			return;
		}
		if (xml_int(body, "ConnectionStatus", &l) && l > 0 && l < 65536)
			s->conn_status = (uint16_t)l;
		if (xml_int(body, "CurrentNetworkType", &l) && l >= 0 && l < NET_NONE)
			s->net_type = (uint8_t)l;
		signal_error[0] = '\0';
		finish_sample();
		return;
	}

	if (xml_text(body, "code", code, sizeof(code))) {
		/* Typically 125002: our cached session expired. Retry once. */
		cookie[0] = '\0';
		if (http.cookie_was_cached) {
			http.stage = STAGE_TOKEN;
			http_start();
			return;
		}
		FAIL("Huawei API error %s", code);
	}

	memset(s, 0, sizeof(*s));
	sample_clear_signal(s);
	if (!xml_u64(body, "CurrentConnectTime", &v) ||
	    !xml_u64(body, "CurrentDownload", &s->download) ||
	    !xml_u64(body, "CurrentUpload", &s->upload))
		FAIL("counters missing in response");
	s->uptime = v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
	xml_u64(body, "TotalDownload", &s->total_download);
	xml_u64(body, "TotalUpload", &s->total_upload);
	s->time = (uint32_t)time(NULL);

	if (!cfg_signal) {
		finish_sample();
		return;
	}
	/* Counters are safe now; signal and status are best effort. */
	http.have_pending = true;
	http.stage = STAGE_SIGNAL;
	http_start();
}

static void http_fd_cb(struct uloop_fd *fd, unsigned int events)
{
	ssize_t n;

	if (http.connecting) {
		int err = 0;
		socklen_t len = sizeof(err);

		if (fd->error || getsockopt(fd->fd, SOL_SOCKET, SO_ERROR, &err, &len) || err)
			FAIL("connect: %s", strerror(err ? err : ECONNREFUSED));
		if (!(events & ULOOP_WRITE))
			return;
		while (http.sent < http.req_len) {
			n = send(fd->fd, http.req + http.sent, http.req_len - http.sent, MSG_NOSIGNAL);
			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0 && errno == EAGAIN)
				return;
			if (n <= 0)
				FAIL("send: %s", strerror(errno));
			http.sent += n;
		}
		http.connecting = false;
		uloop_fd_add(fd, ULOOP_READ);
		return;
	}

	for (;;) {
		size_t cl;
		bool have_cl;
		char *body;

		if (http.len >= sizeof(http.buf) - 1)
			FAIL("response too large");
		n = recv(fd->fd, http.buf + http.len, sizeof(http.buf) - 1 - http.len, 0);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			return;
		if (n < 0)
			FAIL("recv: %s", strerror(errno));
		http.buf[http.len += n] = '\0';
		if (n == 0)
			break;  /* server closed: response complete */
		/* The Huawei web server may keep the connection open. */
		body = http_body(&cl, &have_cl);
		if (body && have_cl && http.len - (size_t)(body - http.buf) >= cl)
			break;
	}
	if (!http.len)
		FAIL("empty response");
	handle_response();
}

static void http_timeout_cb(struct uloop_timeout *t)
{
	(void)t;
	sample_failed("timeout talking to %s", cfg_host);
}

static void http_start(void)
{
	static const char *const paths[] = {
		[STAGE_TOKEN]  = "/api/webserver/SesTokInfo",
		[STAGE_STATS]  = "/api/monitoring/traffic-statistics",
		[STAGE_SIGNAL] = "/api/device/signal",
		[STAGE_STATUS] = "/api/monitoring/status",
	};
	bool auth = http.stage != STAGE_TOKEN;
	int n;

	http_close();
	n = snprintf(http.req, sizeof(http.req),
		"GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n%s%s%s\r\n",
		paths[http.stage], cfg_host,
		auth ? "Cookie: " : "", auth ? cookie : "", auth ? "\r\n" : "");
	if (n < 0 || (size_t)n >= sizeof(http.req))
		FAIL("request too long");
	http.req_len = n;
	http.sent = 0;
	http.len = 0;
	http.buf[0] = '\0';

	http.fd.fd = usock(USOCK_TCP | USOCK_NONBLOCK | USOCK_NUMERIC | USOCK_IPV4ONLY,
	                   cfg_host, cfg_port);
	if (http.fd.fd < 0)
		FAIL("socket/connect: %s", strerror(errno));
	http.fd.cb = http_fd_cb;
	http.connecting = true;
	uloop_fd_add(&http.fd, ULOOP_WRITE);
	http.timeout.cb = http_timeout_cb;
	uloop_timeout_set(&http.timeout, HTTP_TIMEOUT_MS);
}

/* ------------------------------------------------------------------ */
/* Latency: every 10 s one ICMP echo to the internet (alternating      */
/* targets) and one to the Huawei itself (the local, powerline hop)     */
/* ------------------------------------------------------------------ */

#ifndef PING_GAP_MS
#define PING_GAP_MS      10000  /* tests build with a shorter gap */
#endif
#define PING_TIMEOUT_MS  3000
#define PING_SLOTS       8
#ifndef SOL_RAW
#define SOL_RAW 255
#endif
#define ICMP_FILTER_OPT  1          /* linux/icmp.h ICMP_FILTER */

static struct uloop_fd icmp_fd = { .fd = -1 };
static struct uloop_timeout ping_timer;
#define GW_TARGET        PING_MAX_TARGETS  /* ping_addr[] slot of the Huawei */
static struct in_addr ping_addr[PING_MAX_TARGETS + 1];
static bool gw_ping;               /* Huawei probes active */
static uint16_t ping_id, ping_seq;
static unsigned ping_next;
static struct {
	bool used;
	uint8_t target;
	uint16_t seq;
	uint64_t sent_us;
} ping_slot[PING_SLOTS];
static struct {                     /* results since the last sample */
	uint16_t sent, lost, min, max;
	uint32_t rtt_sum, replies;
	uint8_t ok;
} pacc;
static struct {                     /* Huawei probes since the last sample */
	uint16_t sent, lost, max;
	uint32_t rtt_sum, replies;
} gacc;

static uint64_t mono_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static uint16_t inet_csum(const uint8_t *p, size_t n)
{
	uint32_t sum = 0;

	for (; n > 1; p += 2, n -= 2)
		sum += (uint32_t)(p[0] << 8 | p[1]);
	if (n)
		sum += (uint32_t)p[0] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

/* A probe finished: replied (rtt_us >= 0) or timed out (rtt_us < 0). */
static void ping_resolve(unsigned slot, int64_t rtt_us)
{
	ping_slot[slot].used = false;
	if (ping_slot[slot].target == GW_TARGET) {
		if (gacc.sent < 65535)
			gacc.sent++;
		if (rtt_us < 0) {
			if (gacc.lost < 65535)
				gacc.lost++;
		} else {
			uint32_t ms = (uint32_t)((rtt_us + 500) / 1000);
			uint16_t m = ms > 65535 ? 65535 : (uint16_t)(ms ? ms : 1);

			gacc.rtt_sum += m;
			gacc.replies++;
			if (m > gacc.max)
				gacc.max = m;
		}
		return;
	}
	if (pacc.sent < 65535)
		pacc.sent++;
	if (rtt_us < 0) {
		if (pacc.lost < 65535)
			pacc.lost++;
		return;
	}
	{
		uint32_t ms = (uint32_t)((rtt_us + 500) / 1000);
		uint16_t m = ms > 65535 ? 65535 : (uint16_t)(ms ? ms : 1);  /* 0 means "none" */

		pacc.rtt_sum += m;
		pacc.replies++;
		if (!pacc.min || m < pacc.min)
			pacc.min = m;
		if (m > pacc.max)
			pacc.max = m;
		pacc.ok |= (uint8_t)(1u << ping_slot[slot].target);
	}
}

/* Move the results gathered since the last sample into s. */
static void ping_attach(struct sample *s)
{
	if (gacc.sent) {
		s->gw_sent = gacc.sent;
		s->gw_lost = gacc.lost;
		s->gw_rtt_max = gacc.max;
		s->gw_rtt_avg = gacc.replies ? (uint16_t)((gacc.rtt_sum + gacc.replies / 2) / gacc.replies) : 0;
		memset(&gacc, 0, sizeof(gacc));
	}
	if (!cfg_latency || !pacc.sent)
		return;
	s->ping_sent = pacc.sent;
	s->ping_lost = pacc.lost;
	s->ping_ok = pacc.ok;
	s->rtt_min = pacc.min;
	s->rtt_max = pacc.max;
	s->rtt_avg = pacc.replies ? (uint16_t)((pacc.rtt_sum + pacc.replies / 2) / pacc.replies) : 0;
	memset(&pacc, 0, sizeof(pacc));
}

static void icmp_cb(struct uloop_fd *fd, unsigned int events)
{
	uint8_t buf[256];
	struct sockaddr_in from;
	socklen_t fl;
	ssize_t n;
	uint64_t now = mono_us();

	(void)events;
	for (;;) {
		size_t ihl;
		const uint8_t *icmp;
		uint16_t id, seq;
		unsigned k;

		fl = sizeof(from);
		n = recvfrom(fd->fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
		if (n < 0)
			return;
		if (n < 20 || (buf[0] >> 4) != 4)
			continue;
		ihl = (size_t)(buf[0] & 0x0f) * 4;
		if (ihl < 20 || (size_t)n < ihl + 8)
			continue;
		icmp = buf + ihl;
		if (icmp[0] != 0 || icmp[1] != 0)
			continue;  /* echo reply only */
		id = (uint16_t)(icmp[4] << 8 | icmp[5]);
		seq = (uint16_t)(icmp[6] << 8 | icmp[7]);
		if (id != ping_id)
			continue;  /* someone else's ping */
		for (k = 0; k < PING_SLOTS; k++)
			if (ping_slot[k].used && ping_slot[k].seq == seq &&
			    ping_addr[ping_slot[k].target].s_addr == from.sin_addr.s_addr) {
				ping_resolve(k, (int64_t)(now - ping_slot[k].sent_us));
				break;
			}
	}
}

static void ping_send(unsigned target, uint64_t now)
{
	uint8_t pkt[24];
	struct sockaddr_in to;
	unsigned k, slot = PING_SLOTS;
	uint16_t c;

	for (k = 0; k < PING_SLOTS && slot == PING_SLOTS; k++)
		if (!ping_slot[k].used)
			slot = k;
	if (slot == PING_SLOTS || icmp_fd.fd < 0)
		return;
	memset(pkt, 0, sizeof(pkt));
	pkt[0] = 8;  /* echo request */
	pkt[4] = (uint8_t)(ping_id >> 8);
	pkt[5] = (uint8_t)ping_id;
	ping_seq++;
	pkt[6] = (uint8_t)(ping_seq >> 8);
	pkt[7] = (uint8_t)ping_seq;
	memcpy(pkt + 8, "huawei-trafficd", 15);
	c = inet_csum(pkt, sizeof(pkt));
	pkt[2] = (uint8_t)(c >> 8);
	pkt[3] = (uint8_t)c;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_addr = ping_addr[target];
	if (sendto(icmp_fd.fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
		/* No route (e.g. LTE down): a probe that can't leave is a lost probe. */
		ping_slot[slot].target = (uint8_t)target;
		ping_resolve(slot, -1);
		return;
	}
	ping_slot[slot].used = true;
	ping_slot[slot].target = (uint8_t)target;
	ping_slot[slot].seq = ping_seq;
	ping_slot[slot].sent_us = now;
}

static void ping_timer_cb(struct uloop_timeout *t)
{
	uint64_t now = mono_us();
	unsigned k;

	uloop_timeout_set(t, PING_GAP_MS);
	for (k = 0; k < PING_SLOTS; k++)  /* unanswered for too long: lost */
		if (ping_slot[k].used && now - ping_slot[k].sent_us >= PING_TIMEOUT_MS * 1000u)
			ping_resolve(k, -1);
	if (cfg_latency)
		ping_send(ping_next++ % cfg_ntargets, now);
	if (gw_ping)
		ping_send(GW_TARGET, now);
}

static bool ping_open(void)
{
	uint32_t filter = ~(1u << 0);  /* deliver only type 0, echo reply */
	unsigned i;

	for (i = 0; cfg_latency && i < cfg_ntargets; i++)
		if (inet_pton(AF_INET, cfg_targets[i], &ping_addr[i]) != 1) {
			syslog(LOG_ERR, "latency target %s is not an IPv4 address", cfg_targets[i]);
			return false;
		}
	gw_ping = cfg_gw_ping && inet_pton(AF_INET, cfg_host, &ping_addr[GW_TARGET]) == 1;
	if (cfg_gw_ping && !gw_ping)
		syslog(LOG_NOTICE, "Huawei host %s is not an IPv4 address: not pinging it", cfg_host);
	if (!cfg_latency && !gw_ping)
		return false;
	icmp_fd.fd = socket(AF_INET, SOCK_RAW | SOCK_CLOEXEC, IPPROTO_ICMP);
	if (icmp_fd.fd < 0) {
		syslog(LOG_ERR, "ICMP socket: %s (latency disabled)", strerror(errno));
		return false;
	}
	if (setsockopt(icmp_fd.fd, SOL_RAW, ICMP_FILTER_OPT, &filter, sizeof(filter)) < 0)
		syslog(LOG_NOTICE, "ICMP filter not available: %s", strerror(errno));
	ping_id = (uint16_t)(getpid() ^ 0x4854);
	icmp_fd.cb = icmp_cb;
	uloop_fd_add(&icmp_fd, ULOOP_READ);
	ping_timer.cb = ping_timer_cb;
	uloop_timeout_set(&ping_timer, 2500);  /* first probe soon after start */
	return true;
}

/* ------------------------------------------------------------------ */
/* Powerline PHY rate: HomePlug AV CM_NW_STATS from the coordinator    */
/* ------------------------------------------------------------------ */
/*
 * Once per sample, one CM_NW_STATS.REQ (MMV 2, HomePlug AV 2.0) to the
 * powerline network's coordinator (CCo). Its CNF lists every other station
 * with the average PHY rate it sends to it (TX) and receives from it (RX),
 * 16-bit little-endian Mbit/s in the MMV 2 format (the MMV 1 format has one
 * byte each and saturates at 255). We keep the entry of our own station:
 * TX is towards us (down), RX from us (up). Read-only.
 */
#define ETH_P_HPAV       0x88E1
#define MMT_NW_STATS     0x6048     /* .REQ; .CNF = +1 */
#define MMT_MME_ERROR    0x6046     /* CM_MME_ERROR.IND */
#define PLC_MISSES       3          /* unanswered requests in a row before it's an error */

static struct uloop_fd plc_fd = { .fd = -1 };
static int plc_ifindex;
static uint8_t plc_me[6], plc_cco[6], plc_sta[6];
static struct {                     /* readings since the last sample */
	uint32_t down, up;
	uint16_t n;
} placc;
static bool plc_waiting;            /* a request is unanswered */
static unsigned plc_misses;
static uint32_t plc_last_ok;        /* time of the last reading */
static uint16_t plc_last_down, plc_last_up;
static char plc_error[112];

static bool parse_mac(const char *s, uint8_t *m)
{
	unsigned v[6], i;
	char c;

	if (!s || sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &c) != 6)
		return false;
	for (i = 0; i < 6; i++)
		m[i] = (uint8_t)v[i];
	return true;
}

static void plc_attach(struct sample *s)
{
	if (!placc.n)
		return;
	s->plc_down = (uint16_t)((placc.down + placc.n / 2) / placc.n);
	s->plc_up = (uint16_t)((placc.up + placc.n / 2) / placc.n);
	s->plc_n = placc.n;
	memset(&placc, 0, sizeof(placc));
}

static void plc_reply(const uint8_t *b, size_t n)
{
	unsigned mmtype, count, i;
	const uint8_t *e;

	if (n < 20 || memcmp(b + 6, plc_cco, 6) || memcmp(b, plc_me, 6))
		return;  /* not from the coordinator to us */
	mmtype = b[15] | b[16] << 8;
	if (mmtype == MMT_MME_ERROR) {
		snprintf(plc_error, sizeof(plc_error), "coordinator refused the request "
		         "(is %02x:%02x:%02x:%02x:%02x:%02x still the coordinator?)",
		         plc_cco[0], plc_cco[1], plc_cco[2], plc_cco[3], plc_cco[4], plc_cco[5]);
		plc_waiting = false;
		return;
	}
	if (mmtype != MMT_NW_STATS + 1 || b[14] != 2)
		return;
	plc_waiting = false;
	plc_misses = 0;
	count = b[19];
	for (i = 0, e = b + 20; i < count && e + 10 <= b + n; i++, e += 10) {
		unsigned down = e[6] | e[7] << 8, up = e[8] | e[9] << 8;

		if (memcmp(e, plc_sta, 6))
			continue;
		if (!down && !up) {  /* no traffic between them lately: no rate */
			snprintf(plc_error, sizeof(plc_error), "no current rate for our station");
			return;
		}
		placc.down += down;
		placc.up += up;
		if (placc.n < 65535)
			placc.n++;
		plc_last_down = (uint16_t)down;
		plc_last_up = (uint16_t)up;
		plc_last_ok = (uint32_t)time(NULL);
		plc_error[0] = '\0';
		return;
	}
	snprintf(plc_error, sizeof(plc_error), "station %02x:%02x:%02x:%02x:%02x:%02x isn't in the coordinator's list",
	         plc_sta[0], plc_sta[1], plc_sta[2], plc_sta[3], plc_sta[4], plc_sta[5]);
}

static void plc_cb(struct uloop_fd *fd, unsigned int events)
{
	uint8_t b[1600];
	ssize_t n;

	(void)events;
	while ((n = recv(fd->fd, b, sizeof(b), 0)) > 0)
		plc_reply(b, (size_t)n);
}

static void plc_request(void)
{
	uint8_t f[60] = { 0 };
	struct sockaddr_ll sa = { .sll_family = AF_PACKET, .sll_ifindex = plc_ifindex, .sll_halen = 6 };

	if (plc_fd.fd < 0)
		return;
	if (plc_waiting && ++plc_misses >= PLC_MISSES)
		snprintf(plc_error, sizeof(plc_error), "no answer from the coordinator (%u requests)", plc_misses);
	memcpy(f, plc_cco, 6);
	memcpy(f + 6, plc_me, 6);
	f[12] = ETH_P_HPAV >> 8;
	f[13] = ETH_P_HPAV & 0xff;
	f[14] = 2;                       /* MMV: HomePlug AV 2.0 */
	f[15] = MMT_NW_STATS & 0xff;     /* MMTYPE, little-endian */
	f[16] = MMT_NW_STATS >> 8;
	/* f[17..18]: fragmentation info 0 = not fragmented; no payload */
	memcpy(sa.sll_addr, plc_cco, 6);
	if (sendto(plc_fd.fd, f, sizeof(f), 0, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		snprintf(plc_error, sizeof(plc_error), "send on %s: %s", cfg_plc_iface, strerror(errno));
	else
		plc_waiting = true;
}

static bool plc_open(void)
{
	struct ifreq ifr;
	struct sockaddr_ll sa = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_HPAV) };

	if (!parse_mac(cfg_plc_cco, plc_cco) || !parse_mac(cfg_plc_sta, plc_sta)) {
		syslog(LOG_ERR, "powerline: coordinator and station must be MAC addresses (aa:bb:cc:dd:ee:ff)");
		return false;
	}
	plc_fd.fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, htons(ETH_P_HPAV));
	if (plc_fd.fd < 0) {
		snprintf(plc_error, sizeof(plc_error), "socket: %s", strerror(errno));
		goto fail;
	}
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", cfg_plc_iface);
	if (ioctl(plc_fd.fd, SIOCGIFINDEX, &ifr) < 0 || (plc_ifindex = ifr.ifr_ifindex,
	    ioctl(plc_fd.fd, SIOCGIFHWADDR, &ifr) < 0)) {
		snprintf(plc_error, sizeof(plc_error), "interface %s: %s", cfg_plc_iface, strerror(errno));
		goto fail;
	}
	memcpy(plc_me, ifr.ifr_hwaddr.sa_data, 6);
	sa.sll_ifindex = plc_ifindex;
	if (bind(plc_fd.fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		snprintf(plc_error, sizeof(plc_error), "bind %s: %s", cfg_plc_iface, strerror(errno));
		goto fail;
	}
	plc_fd.cb = plc_cb;
	uloop_fd_add(&plc_fd, ULOOP_READ);
	return true;
fail:
	syslog(LOG_ERR, "powerline rate disabled: %s", plc_error);
	if (plc_fd.fd >= 0)
		close(plc_fd.fd);
	plc_fd.fd = -1;
	return false;
}

static struct uloop_timeout sample_timer;

static void sample_timer_cb(struct uloop_timeout *t)
{
	uloop_timeout_set(t, fine.interval * 1000);
	if (http.busy)
		return;
	last_attempt = (uint32_t)time(NULL);
	if (last_attempt < MIN_VALID_TIME) {
		snprintf(last_error, sizeof(last_error), "system clock not set (waiting for NTP)");
		return;
	}
	plc_request();  /* answered within milliseconds, before the HTTP sample ends */
	http.busy = true;
	http.have_pending = false;
	http.stage = cookie[0] ? STAGE_STATS : STAGE_TOKEN;
	http.cookie_was_cached = cookie[0] != '\0';
	http_start();
}

/* ------------------------------------------------------------------ */
/* ubus interface: huawei.traffic                                     */
/* ------------------------------------------------------------------ */

static struct blob_buf b;

static void add_sample(struct blob_buf *buf, const char *name, const struct sample *s)
{
	void *t = blobmsg_open_table(buf, name);

	blobmsg_add_u32(buf, "time", s->time);
	blobmsg_add_u32(buf, "uptime", s->uptime);
	blobmsg_add_u64(buf, "download", s->download);
	blobmsg_add_u64(buf, "upload", s->upload);
	blobmsg_add_u64(buf, "total_download", s->total_download);
	blobmsg_add_u64(buf, "total_upload", s->total_upload);
	if (s->sig_n && s->rsrp != RSRP_NONE) {
		blobmsg_add_u32(buf, "rsrp", (uint32_t)(int32_t)s->rsrp);
		blobmsg_add_u32(buf, "rsrq", (uint32_t)(int32_t)s->rsrq);
		blobmsg_add_u32(buf, "sinr", (uint32_t)(int32_t)s->sinr);
		if (s->rssi != SIG_NONE)
			blobmsg_add_u32(buf, "rssi", (uint32_t)(int32_t)s->rssi);
		blobmsg_add_u32(buf, "signal_samples", s->sig_n);
	}
	if (s->cell_id)
		blobmsg_add_u32(buf, "cell_id", s->cell_id);
	if (s->conn_status)
		blobmsg_add_u32(buf, "conn_status", s->conn_status);
	if (s->net_type != NET_NONE)
		blobmsg_add_u32(buf, "net_type", s->net_type);
	if (s->ping_sent) {
		blobmsg_add_u32(buf, "ping_sent", s->ping_sent);
		blobmsg_add_u32(buf, "ping_lost", s->ping_lost);
		blobmsg_add_u32(buf, "ping_ok", s->ping_ok);
		if (s->ping_lost < s->ping_sent) {
			blobmsg_add_u32(buf, "rtt_min", s->rtt_min);
			blobmsg_add_u32(buf, "rtt_avg", s->rtt_avg);
			blobmsg_add_u32(buf, "rtt_max", s->rtt_max);
		}
	}
	if (s->gw_sent) {
		blobmsg_add_u32(buf, "gw_sent", s->gw_sent);
		blobmsg_add_u32(buf, "gw_lost", s->gw_lost);
		if (s->gw_lost < s->gw_sent) {
			blobmsg_add_u32(buf, "gw_rtt_avg", s->gw_rtt_avg);
			blobmsg_add_u32(buf, "gw_rtt_max", s->gw_rtt_max);
		}
	}
	if (s->plc_n) {
		blobmsg_add_u32(buf, "plc_down", s->plc_down);
		blobmsg_add_u32(buf, "plc_up", s->plc_up);
		blobmsg_add_u32(buf, "plc_samples", s->plc_n);
	}
	blobmsg_close_table(buf, t);
}

static int m_latest(struct ubus_context *ctx, struct ubus_object *obj,
                    struct ubus_request_data *req, const char *method,
                    struct blob_attr *msg)
{
	(void)obj; (void)method; (void)msg;
	if (!fine.count)
		return UBUS_STATUS_NO_DATA;
	blob_buf_init(&b, 0);
	add_sample(&b, "sample", ring_at(&fine, fine.count - 1));
	return ubus_send_reply(ctx, req, b.head);
}

enum { Q_FROM, Q_TO, Q_LIMIT, Q_TIER, __Q_MAX };
static const struct blobmsg_policy query_policy[__Q_MAX] = {
	[Q_FROM]  = { .name = "from",  .type = BLOBMSG_TYPE_INT32 },
	[Q_TO]    = { .name = "to",    .type = BLOBMSG_TYPE_INT32 },
	[Q_LIMIT] = { .name = "limit", .type = BLOBMSG_TYPE_INT32 },
	[Q_TIER]  = { .name = "tier",  .type = BLOBMSG_TYPE_STRING },
};

/* Absolute Unix time, or (if negative) seconds relative to now. */
static int64_t abs_time(struct blob_attr *a, int64_t dflt, int64_t now)
{
	int32_t v;

	if (!a)
		return dflt;
	v = (int32_t)blobmsg_get_u32(a);
	return v < 0 ? now + v : v;
}

struct query_ctx {
	int64_t from, to;
	unsigned n, limit;
	bool truncated;
};

/* Add r's samples with from <= time <= to and time < before. */
static void query_ring(struct query_ctx *q, const struct ring *r, int64_t before)
{
	unsigned i;

	for (i = 0; i < r->count && !q->truncated; i++) {
		const struct sample *s = ring_at(r, i);

		if (s->time < q->from || s->time > q->to || s->time >= before)
			continue;
		if (q->n == q->limit) {
			q->truncated = true;  /* oldest first; continue with from=last.time+1 */
			break;
		}
		add_sample(&b, NULL, s);
		q->n++;
	}
}

static int m_query(struct ubus_context *ctx, struct ubus_object *obj,
                   struct ubus_request_data *req, const char *method,
                   struct blob_attr *msg)
{
	struct blob_attr *tb[__Q_MAX];
	struct query_ctx q = { .limit = QUERY_DEFAULT_MAX };
	int64_t now = time(NULL);
	const char *tier = "auto";
	void *arr;

	(void)obj; (void)method;
	blobmsg_parse(query_policy, __Q_MAX, tb, blob_data(msg), blob_len(msg));
	q.from = abs_time(tb[Q_FROM], 0, now);
	q.to = abs_time(tb[Q_TO], INT64_MAX, now);
	if (tb[Q_LIMIT]) {
		int32_t l = (int32_t)blobmsg_get_u32(tb[Q_LIMIT]);
		if (l < 1)
			return UBUS_STATUS_INVALID_ARGUMENT;
		q.limit = l > QUERY_HARD_MAX ? QUERY_HARD_MAX : (unsigned)l;
	}
	if (tb[Q_TIER])
		tier = blobmsg_get_string(tb[Q_TIER]);
	if (strcmp(tier, "auto") && strcmp(tier, "fine") && strcmp(tier, "coarse"))
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (!strcmp(tier, "coarse") && !coarse.interval)
		return UBUS_STATUS_NOT_SUPPORTED;

	blob_buf_init(&b, 0);
	arr = blobmsg_open_array(&b, "samples");
	if (!strcmp(tier, "fine")) {
		query_ring(&q, &fine, INT64_MAX);
	} else if (!strcmp(tier, "coarse")) {
		query_ring(&q, &coarse, INT64_MAX);
	} else {
		/* auto: coarse samples older than the fine tier, then fine ones. */
		int64_t boundary = fine.count ? ring_at(&fine, 0)->time : INT64_MAX;

		if (coarse.interval)
			query_ring(&q, &coarse, boundary);
		query_ring(&q, &fine, INT64_MAX);
	}
	blobmsg_close_array(&b, arr);
	blobmsg_add_u32(&b, "count", q.n);
	blobmsg_add_u8(&b, "truncated", q.truncated);
	return ubus_send_reply(ctx, req, b.head);
}

static void add_ring_status(const struct ring *r)
{
	void *t = blobmsg_open_table(&b, r->name);

	blobmsg_add_u32(&b, "interval", r->interval);
	blobmsg_add_u32(&b, "capacity", r->cap);
	blobmsg_add_u32(&b, "count", r->count);
	if (r->count) {
		blobmsg_add_u32(&b, "oldest", ring_at(r, 0)->time);
		blobmsg_add_u32(&b, "newest", ring_at(r, r->count - 1)->time);
	}
	if (r->state)
		blobmsg_add_string(&b, "state_file", r->state);
	blobmsg_close_table(&b, t);
}

static int m_status(struct ubus_context *ctx, struct ubus_object *obj,
                    struct ubus_request_data *req, const char *method,
                    struct blob_attr *msg)
{
	(void)obj; (void)method; (void)msg;
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "source", cfg_host);
	add_ring_status(&fine);
	if (coarse.interval)
		add_ring_status(&coarse);
	blobmsg_add_u32(&b, "now", (uint32_t)time(NULL));
	blobmsg_add_u32(&b, "last_attempt", last_attempt);
	blobmsg_add_u32(&b, "last_success", last_success);
	blobmsg_add_u32(&b, "failures", failures);
	if (last_error[0])
		blobmsg_add_string(&b, "last_error", last_error);
	blobmsg_add_u8(&b, "signal", cfg_signal);
	{
		void *t = blobmsg_open_table(&b, "latency"), *a;
		unsigned i;

		blobmsg_add_u8(&b, "enabled", cfg_latency && icmp_fd.fd >= 0);
		blobmsg_add_u32(&b, "interval", PING_GAP_MS / 1000);
		a = blobmsg_open_array(&b, "targets");
		for (i = 0; cfg_latency && i < cfg_ntargets; i++)
			blobmsg_add_string(&b, NULL, cfg_targets[i]);
		blobmsg_close_array(&b, a);
		blobmsg_add_u8(&b, "huawei", gw_ping && icmp_fd.fd >= 0);
		blobmsg_close_table(&b, t);
	}
	if (cfg_flash_dir || flash_error[0]) {
		void *t = blobmsg_open_table(&b, "flash");
		unsigned months[64], n = cfg_flash_dir ? flash_months(months, 64) : 0, i;
		uint64_t bytes = 0;
		char path[PATH_MAX];
		struct stat st;

		for (i = 0; i < n; i++)
			if (month_file(path, sizeof(path), months[i]) && stat(path, &st) == 0)
				bytes += (uint64_t)st.st_size;
		blobmsg_add_u8(&b, "enabled", cfg_flash_dir != NULL);
		if (cfg_flash_dir) {
			blobmsg_add_string(&b, "dir", cfg_flash_dir);
			blobmsg_add_u32(&b, "every", cfg_flash_every);
			blobmsg_add_u32(&b, "files", n);
			blobmsg_add_u64(&b, "bytes", bytes);
			blobmsg_add_u32(&b, "saved_until", flushed_until);
			blobmsg_add_u32(&b, "last_write", flash_last_write);
		}
		if (flash_error[0])
			blobmsg_add_string(&b, "error", flash_error);
		blobmsg_close_table(&b, t);
	}
	if (cfg_signal && signal_error[0])
		blobmsg_add_string(&b, "signal_error", signal_error);
	if (cfg_plc_cco || cfg_plc_sta) {
		void *t = blobmsg_open_table(&b, "powerline");

		blobmsg_add_u8(&b, "enabled", plc_fd.fd >= 0);
		blobmsg_add_string(&b, "interface", cfg_plc_iface);
		if (cfg_plc_cco)
			blobmsg_add_string(&b, "coordinator", cfg_plc_cco);
		if (cfg_plc_sta)
			blobmsg_add_string(&b, "station", cfg_plc_sta);
		if (plc_last_ok) {
			blobmsg_add_u32(&b, "last_reading", plc_last_ok);
			blobmsg_add_u32(&b, "last_down", plc_last_down);
			blobmsg_add_u32(&b, "last_up", plc_last_up);
		}
		if (plc_error[0])
			blobmsg_add_string(&b, "error", plc_error);
		blobmsg_close_table(&b, t);
	}
	return ubus_send_reply(ctx, req, b.head);
}

static const struct ubus_method methods[] = {
	UBUS_METHOD_NOARG("latest", m_latest),
	UBUS_METHOD("query", m_query, query_policy),
	UBUS_METHOD_NOARG("status", m_status),
};

#ifndef OBJ_NAME
#define OBJ_NAME "huawei.traffic"  /* tests build with another name */
#endif
static struct ubus_object_type obj_type = UBUS_OBJECT_TYPE(OBJ_NAME, methods);

static struct ubus_object ubus_obj = {
	.name = OBJ_NAME,
	.type = &obj_type,
	.methods = methods,
	.n_methods = ARRAY_SIZE(methods),
};

static bool ubus_ever_connected;

static void ubus_connected(struct ubus_context *ctx)
{
	int ret = ubus_add_object(ctx, &ubus_obj);

	ubus_ever_connected = true;
	if (ret)
		syslog(LOG_ERR, "ubus_add_object: %s", ubus_strerror(ret));
}

static struct ubus_auto_conn conn;

/* ------------------------------------------------------------------ */

static unsigned parse_uint(const char *s, const char *what, unsigned min, unsigned max)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno || !*s || *end || v < min || v > max) {
		fprintf(stderr, "invalid %s: %s (allowed %u-%u)\n", what, s, min, max);
		exit(2);
	}
	return v;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -H host      Huawei router IPv4 address (default 192.168.0.1)\n"
		"  -P port      HTTP port (default 80)\n"
		"  -i seconds   sampling interval (default 60)\n"
		"  -n samples   fine history size (default 4320, 80 bytes each)\n"
		"  -c seconds   coarse tier interval (default 3600, 0 disables it)\n"
		"  -N samples   coarse history size (default 8760, 80 bytes each)\n"
		"  -f file      fine history file: loaded at start, saved on exit\n"
		"  -F file      coarse history file (default: <fine file>.coarse)\n"
		"  -S seconds   also save the history files this often (default: exit only)\n"
		"  -D dir       keep the hourly history on flash in dir (compressed, monthly files)\n"
		"  -E seconds   append to the flash history this often (default 86400)\n"
		"  -G           don't read signal quality and connection status\n"
		"  -L           don't measure internet latency\n"
		"  -g           don't measure latency to the Huawei itself (the local link)\n"
		"  -T address   latency target (repeatable, up to 4; default 1.1.1.1 and 8.8.8.8)\n"
		"  -A mac       powerline rate: the HomePlug AV coordinator to ask (with -B)\n"
		"  -B mac       powerline rate: our own powerline adapter (the station to report)\n"
		"  -I iface     powerline rate: interface to ask on (default br-lan)\n"
		"  -s socket    ubus socket path\n", prog);
	exit(2);
}

int main(int argc, char **argv)
{
	int opt;

	static char coarse_default[PATH_MAX];

	while ((opt = getopt(argc, argv, "H:P:i:n:c:N:f:F:S:s:GD:E:LgT:A:B:I:h")) != -1) {
		switch (opt) {
		case 'H': cfg_host = optarg; break;
		case 'P': cfg_port = optarg; break;
		case 'i': fine.interval = parse_uint(optarg, "interval", 5, 86400); break;
		case 'n': fine.cap = parse_uint(optarg, "history size", 1, 1000000); break;
		case 'c': coarse.interval = parse_uint(optarg, "coarse interval", 0, 604800); break;
		case 'N': coarse.cap = parse_uint(optarg, "coarse history size", 1, 1000000); break;
		case 'f': fine.state = optarg; break;
		case 'F': coarse.state = optarg; break;
		case 'S': cfg_save_every = parse_uint(optarg, "save interval", 60, 604800); break;
		case 's': cfg_socket = optarg; break;
		case 'G': cfg_signal = false; break;
		case 'L': cfg_latency = false; break;
		case 'g': cfg_gw_ping = false; break;
		case 'T': {
			static bool custom;

			if (!custom) {  /* the first -T replaces the defaults */
				custom = true;
				cfg_ntargets = 0;
			}
			if (cfg_ntargets == PING_MAX_TARGETS) {
				fprintf(stderr, "at most %d latency targets\n", PING_MAX_TARGETS);
				return 2;
			}
			cfg_targets[cfg_ntargets++] = optarg;
			break;
		}
		case 'A': cfg_plc_cco = *optarg ? optarg : NULL; break;
		case 'B': cfg_plc_sta = *optarg ? optarg : NULL; break;
		case 'I': cfg_plc_iface = optarg; break;
		case 'D': cfg_flash_dir = *optarg ? optarg : NULL; break;
		case 'E': cfg_flash_every = parse_uint(optarg, "flash interval", 300, 30 * 86400); break;
		default: usage(argv[0]);
		}
	}
	if (optind != argc)
		usage(argv[0]);
	if (coarse.interval && coarse.interval <= fine.interval) {
		fprintf(stderr, "coarse interval must be longer than the sampling interval (or 0)\n");
		return 2;
	}
	if (coarse.interval && !coarse.state && fine.state &&
	    snprintf(coarse_default, sizeof(coarse_default), "%s.coarse", fine.state) <
	    (int)sizeof(coarse_default))
		coarse.state = coarse_default;

	openlog("huawei-trafficd", LOG_PID | LOG_PERROR, LOG_DAEMON);
	fine.buf = calloc(fine.cap, sizeof(struct sample));
	coarse.buf = coarse.interval ? calloc(coarse.cap, sizeof(struct sample)) : NULL;
	if (!fine.buf || (coarse.interval && !coarse.buf)) {
		syslog(LOG_ERR, "cannot allocate history");
		return 1;
	}
	signal(SIGPIPE, SIG_IGN);
	http.fd.fd = -1;
	if (cfg_flash_dir && (!coarse.interval || !flash_dir_ok())) {
		if (!coarse.interval)
			snprintf(flash_error, sizeof(flash_error), "needs the coarse tier; disabled");
		syslog(LOG_WARNING, "flash history: %s", flash_error);
		cfg_flash_dir = NULL;
	}
	state_load();

	uloop_init();
	conn.path = cfg_socket;
	conn.cb = ubus_connected;
	ubus_auto_connect(&conn);

	sample_timer.cb = sample_timer_cb;
	uloop_timeout_set(&sample_timer, 1000);
	if ((cfg_latency || cfg_gw_ping) && !ping_open())
		cfg_latency = gw_ping = false;
	if (cfg_plc_cco || cfg_plc_sta) {
		if (!cfg_plc_cco || !cfg_plc_sta)
			snprintf(plc_error, sizeof(plc_error), "needs both the coordinator (-A) and our station (-B)");
		else if (plc_open())
			syslog(LOG_INFO, "powerline rate: asking %s on %s for station %s",
			       cfg_plc_cco, cfg_plc_iface, cfg_plc_sta);
	}
	if (cfg_flash_dir) {
		flash_timer.cb = flash_timer_cb;
		uloop_timeout_set(&flash_timer, cfg_flash_every * 1000);
	}
	if ((fine.state || coarse.state) && cfg_save_every) {
		save_timer.cb = save_timer_cb;
		uloop_timeout_set(&save_timer, cfg_save_every * 1000);
	}
	syslog(LOG_INFO, "sampling %s:%s every %us, keeping %u samples; coarse tier: "
	       "%u samples every %us", cfg_host, cfg_port, fine.interval, fine.cap,
	       coarse.interval ? coarse.cap : 0, coarse.interval);

	uloop_run();  /* until SIGTERM/SIGINT */

	http_close();
	flash_flush();
	state_save();
	if (ubus_ever_connected)  /* the context is only valid once connected */
		ubus_shutdown(&conn.ctx);
	uloop_done();
	free(fine.buf);
	free(coarse.buf);
	return 0;
}
