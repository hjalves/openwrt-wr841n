/*
 * tinymdns: a minimal mDNS responder / DNS-SD advertiser (RFC 6762, 6763)
 * for a small OpenWrt router.
 *
 * Answers <host>.local (A), the reverse name of its IPv4 address (PTR) and
 * a few configured DNS-SD services (PTR, SRV, TXT). It probes for name
 * conflicts before claiming names, announces at start, sends goodbyes on
 * stop, answers legacy unicast and QU queries, rate-limits multicast, and
 * re-probes when the interface address changes.
 *
 * Every received packet is untrusted: all parsing is bounds-checked.
 *
 * Deliberately left out: IPv6, known-answer suppression, the random
 * 20-120 ms delay for shared answers, simultaneous-probe tie-breaking,
 * and name compression in packets we send. These don't matter for a
 * single advertiser on a home LAN.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <libubox/uloop.h>

#define MDNS_PORT    5353
#define MDNS_GROUP   "224.0.0.251"
#define MAX_SVC      4
#define MAX_WIRE     256              /* max encoded DNS name, incl. root */
#define PKT_MAX      1460
#define TTL_HOST     120              /* RFC 6762 §10: host records */
#define TTL_OTHER    4500             /* everything else */
#define TYPE_A       1
#define TYPE_PTR     12
#define TYPE_TXT     16
#define TYPE_SRV     33
#define TYPE_ANY     255
#define CLASS_IN     1
#define FLUSH_BIT    0x8000           /* cache-flush (responses), QU (questions) */

/* ---- names: kept in uncompressed wire format (length-prefixed labels) ---- */

struct name {
	uint8_t w[MAX_WIRE];
	size_t len;
};

/* Append labels from a dotted string; a label may not contain '.'. */
static bool name_append(struct name *n, const char *dotted)
{
	const char *p = dotted;

	if (n->len && n->w[n->len - 1] == 0)
		n->len--;  /* drop the root to extend */
	while (*p) {
		size_t l = strcspn(p, ".");

		if (l == 0 || l > 63 || n->len + 1 + l + 1 > MAX_WIRE)
			return false;
		n->w[n->len++] = (uint8_t)l;
		memcpy(n->w + n->len, p, l);
		n->len += l;
		p += l;
		if (*p == '.')
			p++;
	}
	n->w[n->len++] = 0;
	return true;
}

/* Prepend one label (which may contain dots, e.g. an instance name). */
static bool name_label_plus(struct name *out, const char *label, const struct name *rest)
{
	size_t l = strlen(label);

	if (l == 0 || l > 63 || 1 + l + rest->len > MAX_WIRE)
		return false;
	out->w[0] = (uint8_t)l;
	memcpy(out->w + 1, label, l);
	memcpy(out->w + 1 + l, rest->w, rest->len);
	out->len = 1 + l + rest->len;
	return true;
}

static bool name_eq(const struct name *a, const struct name *b)
{
	size_t i;

	if (a->len != b->len)
		return false;
	for (i = 0; i < a->len; i++)  /* lengths match case-insensitively too */
		if (tolower(a->w[i]) != tolower(b->w[i]))
			return false;
	return true;
}

/*
 * Read a possibly compressed name at *off into out. Pointers must point
 * strictly backwards, which also rules out loops.
 */
static bool name_read(const uint8_t *pkt, size_t plen, size_t *off, struct name *out)
{
	size_t pos = *off, next = 0;
	bool jumped = false;

	out->len = 0;
	for (;;) {
		uint8_t l;

		if (pos >= plen)
			return false;
		l = pkt[pos];
		if ((l & 0xc0) == 0xc0) {
			size_t target;

			if (pos + 1 >= plen)
				return false;
			target = (size_t)(l & 0x3f) << 8 | pkt[pos + 1];
			if (target >= pos)
				return false;
			if (!jumped) {
				next = pos + 2;
				jumped = true;
			}
			pos = target;
			continue;
		}
		if (l & 0xc0)
			return false;  /* reserved label types */
		if (pos + 1 + l > plen || out->len + 1 + l > MAX_WIRE)
			return false;
		memcpy(out->w + out->len, pkt + pos, 1 + l);
		out->len += 1 + l;
		pos += 1 + l;
		if (l == 0)
			break;
	}
	*off = jumped ? next : pos;
	return true;
}

/* ---- configuration and state ---- */

struct svc {
	char inst[64], type[64];  /* "LTE data usage", "_http._tcp" */
	uint16_t port;
	uint8_t txt[256];
	size_t txt_len;
	struct name inst_name, type_name;
};

static const char *ifname = "br-lan";
static char host_base[64] = "openwrt", host[64];
static struct svc svcs[MAX_SVC];
static unsigned nsvc;
static unsigned renames;

static int ifindex;
static struct in_addr ip;          /* current IPv4 of ifname, 0 = none */
static struct name host_name, rev_name, services_name;

enum state { IDLE, PROBING, ANNOUNCING, RUNNING };
static enum state state = IDLE;
static unsigned step;              /* probe / announce counter */

/* ---- records ---- */

enum rkind { R_A, R_REV, R_SVCS, R_PTR, R_SRV, R_TXT };

struct rr {
	enum rkind kind;
	int svc;                       /* index into svcs, or -1 */
	const struct name *name;
	uint16_t type;
	bool unique;                   /* cache-flush bit allowed */
	uint32_t ttl;
	uint64_t last_mcast_ms;
};

static struct rr rrs[2 + 4 * MAX_SVC];
static unsigned nrr;

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void rr_add(enum rkind kind, int svc, const struct name *n, uint16_t type,
                   bool unique, uint32_t ttl)
{
	unsigned i;

	for (i = 0; i < nrr; i++)  /* two services of one type share a record */
		if (rrs[i].kind == kind && kind == R_SVCS && name_eq(rrs[i].name, n) &&
		    name_eq(&svcs[rrs[i].svc].type_name, &svcs[svc].type_name))
			return;
	rrs[nrr++] = (struct rr){ kind, svc, n, type, unique, ttl, 0 };
}

static bool build_names(void)
{
	char rev[40];
	const uint8_t *b = (const uint8_t *)&ip.s_addr;
	unsigned i;

	memset(&host_name, 0, sizeof(host_name));
	memset(&rev_name, 0, sizeof(rev_name));
	memset(&services_name, 0, sizeof(services_name));
	snprintf(rev, sizeof(rev), "%u.%u.%u.%u.in-addr.arpa", b[3], b[2], b[1], b[0]);
	if (!name_append(&host_name, host) || !name_append(&host_name, "local") ||
	    !name_append(&rev_name, rev) || !name_append(&services_name, "_services._dns-sd._udp.local"))
		return false;
	nrr = 0;
	rr_add(R_A, -1, &host_name, TYPE_A, true, TTL_HOST);
	rr_add(R_REV, -1, &rev_name, TYPE_PTR, true, TTL_HOST);
	for (i = 0; i < nsvc; i++) {
		struct svc *s = &svcs[i];
		struct name t = { .len = 0 };

		if (!name_append(&t, s->type) || !name_append(&t, "local"))
			return false;
		s->type_name = t;
		if (!name_label_plus(&s->inst_name, s->inst, &s->type_name))
			return false;
		rr_add(R_SVCS, (int)i, &services_name, TYPE_PTR, false, TTL_OTHER);
		rr_add(R_PTR, (int)i, &s->type_name, TYPE_PTR, false, TTL_OTHER);
		rr_add(R_SRV, (int)i, &s->inst_name, TYPE_SRV, true, TTL_HOST);
		rr_add(R_TXT, (int)i, &s->inst_name, TYPE_TXT, true, TTL_OTHER);
	}
	return true;
}

/* ---- packet building ---- */

struct pkt {
	uint8_t b[PKT_MAX];
	size_t len;
	bool full;
};

static void put(struct pkt *p, const void *d, size_t n)
{
	if (p->full || p->len + n > sizeof(p->b)) {
		p->full = true;
		return;
	}
	memcpy(p->b + p->len, d, n);
	p->len += n;
}

static void put16(struct pkt *p, uint16_t v)
{
	uint8_t x[2] = { v >> 8, v & 0xff };

	put(p, x, 2);
}

static void put32(struct pkt *p, uint32_t v)
{
	uint8_t x[4] = { v >> 24, v >> 16 & 0xff, v >> 8 & 0xff, v & 0xff };

	put(p, x, 4);
}

static void hdr(struct pkt *p, uint16_t id, uint16_t flags, uint16_t qd, uint16_t an,
                uint16_t ns, uint16_t ar)
{
	p->len = 0;
	p->full = false;
	put16(p, id);
	put16(p, flags);
	put16(p, qd);
	put16(p, an);
	put16(p, ns);
	put16(p, ar);
}

static void set_count(struct pkt *p, int which, uint16_t v)  /* 0=qd 1=an 2=ns 3=ar */
{
	p->b[4 + 2 * which] = v >> 8;
	p->b[5 + 2 * which] = v & 0xff;
}

static void put_rr(struct pkt *p, const struct rr *r, uint32_t ttl, bool flush)
{
	size_t rdlen_at;
	uint16_t rdlen;

	put(p, r->name->w, r->name->len);
	put16(p, r->type);
	put16(p, CLASS_IN | (flush && r->unique ? FLUSH_BIT : 0));
	put32(p, ttl);
	rdlen_at = p->len;
	put16(p, 0);
	switch (r->kind) {
	case R_A:
		put(p, &ip.s_addr, 4);
		break;
	case R_REV:
		put(p, host_name.w, host_name.len);
		break;
	case R_SVCS:
		put(p, svcs[r->svc].type_name.w, svcs[r->svc].type_name.len);
		break;
	case R_PTR:
		put(p, svcs[r->svc].inst_name.w, svcs[r->svc].inst_name.len);
		break;
	case R_SRV:
		put16(p, 0);  /* priority */
		put16(p, 0);  /* weight */
		put16(p, svcs[r->svc].port);
		put(p, host_name.w, host_name.len);
		break;
	case R_TXT:
		if (svcs[r->svc].txt_len)
			put(p, svcs[r->svc].txt, svcs[r->svc].txt_len);
		else
			put(p, "", 1);  /* one empty string */
		break;
	}
	if (p->full)
		return;
	rdlen = (uint16_t)(p->len - rdlen_at - 2);
	p->b[rdlen_at] = rdlen >> 8;
	p->b[rdlen_at + 1] = rdlen & 0xff;
}

/* ---- socket ---- */

static struct uloop_fd sock = { .fd = -1 };
static struct sockaddr_in group;

static void send_to(const struct pkt *p, const struct sockaddr_in *to)
{
	if (p->full) {
		syslog(LOG_WARNING, "reply too large, not sent");
		return;
	}
	if (sendto(sock.fd, p->b, p->len, 0, (const struct sockaddr *)to, sizeof(*to)) < 0 &&
	    errno != ENETUNREACH && errno != ENETDOWN)
		syslog(LOG_WARNING, "sendto: %s", strerror(errno));
}

static void join_group(bool join)
{
	struct ip_mreqn m;

	memset(&m, 0, sizeof(m));
	inet_pton(AF_INET, MDNS_GROUP, &m.imr_multiaddr);
	m.imr_ifindex = ifindex;
	if (setsockopt(sock.fd, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
	               &m, sizeof(m)) < 0 && join)
		syslog(LOG_WARNING, "join %s on %s: %s", MDNS_GROUP, ifname, strerror(errno));
	if (join && setsockopt(sock.fd, IPPROTO_IP, IP_MULTICAST_IF, &m, sizeof(m)) < 0)
		syslog(LOG_WARNING, "IP_MULTICAST_IF: %s", strerror(errno));
}

/* ---- probing, announcing, goodbyes ---- */

static struct uloop_timeout step_timer;

static void send_all(uint32_t ttl_override, bool use_override)
{
	struct pkt p;
	unsigned i;
	uint64_t now = now_ms();

	hdr(&p, 0, 0x8400, 0, (uint16_t)nrr, 0, 0);  /* response, authoritative */
	for (i = 0; i < nrr; i++) {
		put_rr(&p, &rrs[i], use_override ? ttl_override : rrs[i].ttl, true);
		rrs[i].last_mcast_ms = now;
	}
	send_to(&p, &group);
}

static void send_probe(void)
{
	struct pkt p;
	unsigned i, nq = 1, ns = 0;

	hdr(&p, 0, 0, 0, 0, 0, 0);
	put(&p, host_name.w, host_name.len);  /* questions: our unique names, ANY, QU */
	put16(&p, TYPE_ANY);
	put16(&p, CLASS_IN | FLUSH_BIT);
	for (i = 0; i < nsvc; i++, nq++) {
		put(&p, svcs[i].inst_name.w, svcs[i].inst_name.len);
		put16(&p, TYPE_ANY);
		put16(&p, CLASS_IN | FLUSH_BIT);
	}
	for (i = 0; i < nrr; i++)  /* authority: the data we intend to claim */
		if (rrs[i].unique && rrs[i].kind != R_REV) {
			put_rr(&p, &rrs[i], rrs[i].ttl, false);
			ns++;
		}
	set_count(&p, 0, (uint16_t)nq);
	set_count(&p, 2, (uint16_t)ns);
	send_to(&p, &group);
}

static void start_probe(unsigned delay_ms)
{
	if (!ip.s_addr)
		return;
	if (!build_names()) {
		syslog(LOG_ERR, "invalid host or service name");
		state = IDLE;
		return;
	}
	state = PROBING;
	step = 0;
	uloop_timeout_set(&step_timer, delay_ms);
}

static void step_cb(struct uloop_timeout *t)
{
	if (state == PROBING) {
		if (step < 3) {
			send_probe();
			step++;
			uloop_timeout_set(t, 250);
			return;
		}
		state = ANNOUNCING;
		step = 0;
		syslog(LOG_INFO, "claimed %s.local (%s), %u service(s)", host, inet_ntoa(ip), nsvc);
	}
	if (state == ANNOUNCING) {
		send_all(0, false);
		if (++step < 3) {
			uloop_timeout_set(t, 1000 << (step - 1));  /* 1 s, 2 s */
			return;
		}
		state = RUNNING;
	}
}

/* Another host already uses one of our names: pick a new one, probe again. */
static void conflict(const struct name *n)
{
	unsigned i;

	if (++renames > 20) {
		syslog(LOG_ERR, "too many name conflicts, giving up");
		state = IDLE;
		return;
	}
	if (name_eq(n, &host_name)) {
		snprintf(host, sizeof(host), "%.50s-%u", host_base, renames + 1);
		syslog(LOG_NOTICE, "name conflict: now using %s.local", host);
	}
	for (i = 0; i < nsvc; i++)
		if (name_eq(n, &svcs[i].inst_name)) {
			char base[64];
			char *paren = strrchr(svcs[i].inst, '(');

			snprintf(base, sizeof(base), "%.63s", svcs[i].inst);
			if (paren && paren > svcs[i].inst && paren[-1] == ' ')
				base[paren - svcs[i].inst - 1] = '\0';
			snprintf(svcs[i].inst, sizeof(svcs[i].inst), "%.50s (%u)", base, renames + 1);
			syslog(LOG_NOTICE, "service name conflict: now \"%s\"", svcs[i].inst);
		}
	start_probe(1000);  /* RFC 6762 §8.1: wait before probing again */
}

/* ---- incoming packets ---- */

static bool skip_rr(const uint8_t *pkt, size_t len, size_t *off, struct name *n,
                    uint16_t *type, const uint8_t **rdata, uint16_t *rdlen)
{
	if (!name_read(pkt, len, off, n) || *off + 10 > len)
		return false;
	*type = (uint16_t)(pkt[*off] << 8 | pkt[*off + 1]);
	*rdlen = (uint16_t)(pkt[*off + 8] << 8 | pkt[*off + 9]);
	*off += 10;
	if (*off + *rdlen > len)
		return false;
	*rdata = pkt + *off;
	*off += *rdlen;
	return true;
}

/* Is this record data identical to ours (another copy of us, e.g. a reflector)? */
static bool same_data(const struct rr *r, const uint8_t *pkt, size_t len,
                      const uint8_t *rd, uint16_t rdlen)
{
	const struct svc *s = r->svc >= 0 ? &svcs[r->svc] : NULL;
	struct name target;
	size_t off;

	switch (r->kind) {
	case R_A:
		return rdlen == 4 && !memcmp(rd, &ip.s_addr, 4);
	case R_SRV:
		off = (size_t)(rd - pkt) + 6;
		return rdlen > 6 && (rd[4] << 8 | rd[5]) == s->port &&
		       name_read(pkt, len, &off, &target) && name_eq(&target, &host_name);
	case R_TXT:
		return s->txt_len ? rdlen == s->txt_len && !memcmp(rd, s->txt, rdlen)
		                  : rdlen == 1 && rd[0] == 0;
	default:
		return false;
	}
}

/* Responses from other hosts: does any record claim one of our unique names? */
static void handle_response(const uint8_t *pkt, size_t len, unsigned count, size_t off)
{
	unsigned i, j;

	for (i = 0; i < count; i++) {
		struct name n;
		uint16_t type, rdlen;
		const uint8_t *rd;

		if (!skip_rr(pkt, len, &off, &n, &type, &rd, &rdlen))
			return;
		for (j = 0; j < nrr; j++) {
			if (!rrs[j].unique || rrs[j].kind == R_REV || !name_eq(&n, rrs[j].name))
				continue;
			if (type == rrs[j].type && same_data(&rrs[j], pkt, len, rd, rdlen))
				continue;
			if (state == PROBING || type == rrs[j].type) {
				conflict(&n);
				return;
			}
		}
	}
}

static void handle_query(const uint8_t *pkt, size_t len, const struct sockaddr_in *from)
{
	bool legacy = ntohs(from->sin_port) != MDNS_PORT;
	bool want_unicast = legacy;
	bool ans[sizeof(rrs) / sizeof(rrs[0])] = { false }, add[sizeof(ans)] = { false };
	unsigned qd = (unsigned)(pkt[4] << 8 | pkt[5]), i, j, nan = 0, nad = 0;
	size_t off = 12, qstart = 12, qend;
	uint64_t now = now_ms();
	struct pkt p;

	if (qd == 0 || qd > 32)
		return;
	for (i = 0; i < qd; i++) {
		struct name n;
		uint16_t qtype, qclass;

		if (!name_read(pkt, len, &off, &n) || off + 4 > len)
			return;
		qtype = (uint16_t)(pkt[off] << 8 | pkt[off + 1]);
		qclass = (uint16_t)(pkt[off + 2] << 8 | pkt[off + 3]);
		off += 4;
		if ((qclass & 0x7fff) != CLASS_IN && (qclass & 0x7fff) != TYPE_ANY)
			continue;
		if (qclass & FLUSH_BIT)
			want_unicast = true;  /* QU question */
		for (j = 0; j < nrr; j++)
			if ((qtype == rrs[j].type || qtype == TYPE_ANY) && name_eq(&n, rrs[j].name))
				ans[j] = true;
	}
	qend = off;
	/* Additional records (RFC 6763 §12): PTR -> SRV+TXT(+A), SRV -> A. */
	for (j = 0; j < nrr; j++) {
		unsigned k;

		if (!ans[j])
			continue;
		for (k = 0; k < nrr; k++) {
			if (rrs[j].kind == R_PTR && rrs[k].svc == rrs[j].svc &&
			    (rrs[k].kind == R_SRV || rrs[k].kind == R_TXT))
				add[k] = true;
			if ((rrs[j].kind == R_PTR || rrs[j].kind == R_SRV) && rrs[k].kind == R_A)
				add[k] = true;
		}
	}
	/* Multicast: don't repeat a record sent in the last second (RFC 6762 §6). */
	for (j = 0; j < nrr; j++) {
		if (!want_unicast && ans[j] && now - rrs[j].last_mcast_ms < 1000)
			ans[j] = false;
		if (ans[j])
			nan++;
		if (add[j] && !ans[j])
			nad++;
		else
			add[j] = false;
	}
	if (!nan)
		return;

	if (legacy) {  /* RFC 6762 §6.7: answer like a normal DNS server */
		hdr(&p, (uint16_t)(pkt[0] << 8 | pkt[1]), 0x8400, (uint16_t)qd, 0, 0, 0);
		put(&p, pkt + qstart, qend - qstart);  /* echo the questions */
		if (p.full) {  /* questions too big to echo: answer without them */
			hdr(&p, (uint16_t)(pkt[0] << 8 | pkt[1]), 0x8400, 0, 0, 0, 0);
		}
	} else {
		hdr(&p, 0, 0x8400, 0, 0, 0, 0);
	}
	for (j = 0; j < nrr; j++)
		if (ans[j]) {
			put_rr(&p, &rrs[j], legacy && rrs[j].ttl > 10 ? 10 : rrs[j].ttl, !legacy);
			if (!want_unicast)
				rrs[j].last_mcast_ms = now;
		}
	for (j = 0; j < nrr; j++)
		if (add[j])
			put_rr(&p, &rrs[j], legacy && rrs[j].ttl > 10 ? 10 : rrs[j].ttl, !legacy);
	set_count(&p, 1, (uint16_t)nan);
	set_count(&p, 3, (uint16_t)nad);
	send_to(&p, want_unicast ? from : &group);
}

static void sock_cb(struct uloop_fd *fd, unsigned int events)
{
	uint8_t pkt[9000];
	union { char buf[CMSG_SPACE(sizeof(struct in_pktinfo))]; struct cmsghdr align; } cbuf;
	struct sockaddr_in from;
	struct iovec iov = { pkt, sizeof(pkt) };
	struct msghdr msg = { &from, sizeof(from), &iov, 1, cbuf.buf, sizeof(cbuf.buf), 0 };
	struct cmsghdr *c;
	ssize_t n;
	int in_if = -1;

	(void)events;
	for (;;) {
		msg.msg_namelen = sizeof(from);
		msg.msg_controllen = sizeof(cbuf.buf);
		in_if = -1;
		n = recvmsg(fd->fd, &msg, MSG_DONTWAIT);
		if (n < 0)
			return;
		if (msg.msg_flags & MSG_TRUNC)
			continue;
		for (c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
			if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO)
				in_if = ((struct in_pktinfo *)CMSG_DATA(c))->ipi_ifindex;
		/* Only our interface; ignore our own packets; need a full header. */
		if (in_if != ifindex || n < 12 || state == IDLE || from.sin_addr.s_addr == ip.s_addr)
			continue;
		if ((pkt[2] & 0x78) != 0)  /* opcode must be 0 (QUERY) */
			continue;
		if (pkt[2] & 0x80) {  /* response */
			unsigned an = (unsigned)(pkt[6] << 8 | pkt[7]) + (unsigned)(pkt[8] << 8 | pkt[9]) +
			              (unsigned)(pkt[10] << 8 | pkt[11]);
			size_t off = 12;
			unsigned qd = (unsigned)(pkt[4] << 8 | pkt[5]), i;
			struct name skip;

			for (i = 0; i < qd; i++)  /* responses normally carry no questions */
				if (!name_read(pkt, (size_t)n, &off, &skip) || (off += 4) > (size_t)n)
					goto next;
			handle_response(pkt, (size_t)n, an, off);
		} else if (state != PROBING) {
			handle_query(pkt, (size_t)n, &from);
		}
next:
		;
	}
}

/* ---- interface watching ---- */

static struct uloop_timeout if_timer;

static struct in_addr if_addr(void)
{
	struct ifreq r;
	struct in_addr a = { 0 };

	memset(&r, 0, sizeof(r));
	snprintf(r.ifr_name, sizeof(r.ifr_name), "%s", ifname);
	if (ioctl(sock.fd, SIOCGIFADDR, &r) == 0 && r.ifr_addr.sa_family == AF_INET)
		a = ((struct sockaddr_in *)&r.ifr_addr)->sin_addr;
	return a;
}

/* Every 10 s: follow interface (re)creation and address changes. */
static void if_cb(struct uloop_timeout *t)
{
	int idx = (int)if_nametoindex(ifname);
	struct in_addr a = idx ? if_addr() : (struct in_addr){ 0 };

	if (idx != ifindex) {
		if (ifindex)
			join_group(false);
		ifindex = idx;
		if (ifindex)
			join_group(true);
		ip.s_addr = 0;  /* force the address check below */
	}
	if (a.s_addr != ip.s_addr) {
		if (state != IDLE && ip.s_addr)
			send_all(0, true);  /* goodbye for the old address */
		ip = a;
		if (ip.s_addr) {
			renames = 0;
			start_probe(200 + (unsigned)(rand() % 250));  /* RFC 6762 §8.1 */
		} else {
			state = IDLE;
			syslog(LOG_INFO, "%s has no IPv4 address, waiting", ifname);
		}
	}
	uloop_timeout_set(t, 10000);
}

/* ---- setup ---- */

/* "type,port,instance name[,txt entry...]" e.g. "_http._tcp,80,LTE data usage,path=/x" */
static bool parse_service(const char *arg)
{
	char buf[512], *f[12], *p;
	unsigned nf = 0, i;
	unsigned long port;
	struct svc *s;

	if (nsvc >= MAX_SVC || snprintf(buf, sizeof(buf), "%s", arg) >= (int)sizeof(buf))
		return false;
	for (p = strtok(buf, ","); p && nf < 12; p = strtok(NULL, ","))
		f[nf++] = p;
	if (nf < 3)
		return false;
	port = strtoul(f[1], &p, 10);
	if (*p || port < 1 || port > 65535 || strlen(f[0]) >= 64 || strlen(f[2]) >= 64 || !*f[2])
		return false;
	s = &svcs[nsvc];
	memset(s, 0, sizeof(*s));
	snprintf(s->type, sizeof(s->type), "%s", f[0]);
	snprintf(s->inst, sizeof(s->inst), "%s", f[2]);
	s->port = (uint16_t)port;
	for (i = 3; i < nf; i++) {
		size_t l = strlen(f[i]);

		if (l > 255 || s->txt_len + 1 + l > sizeof(s->txt))
			return false;
		s->txt[s->txt_len++] = (uint8_t)l;
		memcpy(s->txt + s->txt_len, f[i], l);
		s->txt_len += l;
	}
	nsvc++;
	return true;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-i interface] [-n hostname] [-s service]...\n"
		"  -i interface  network device to serve (default br-lan)\n"
		"  -n hostname   answer for <hostname>.local (default openwrt)\n"
		"  -s service    \"type,port,instance name[,txt...]\", e.g.\n"
		"                \"_http._tcp,80,LTE data usage,path=/traffic.html\" (up to %d)\n",
		prog, MAX_SVC);
	exit(2);
}

int main(int argc, char **argv)
{
	struct sockaddr_in any;
	int opt, one = 1, ttl = 255;
	unsigned char loop = 0, mttl = 255;
	size_t i;

	while ((opt = getopt(argc, argv, "i:n:s:h")) != -1) {
		switch (opt) {
		case 'i': ifname = optarg; break;
		case 'n':
			if (!*optarg || strlen(optarg) > 50 || strchr(optarg, '.'))
				usage(argv[0]);
			snprintf(host_base, sizeof(host_base), "%s", optarg);
			break;
		case 's':
			if (!parse_service(optarg)) {
				fprintf(stderr, "bad service: %s\n", optarg);
				usage(argv[0]);
			}
			break;
		default: usage(argv[0]);
		}
	}
	if (optind != argc)
		usage(argv[0]);
	for (i = 0; host_base[i]; i++)
		host_base[i] = (char)tolower((unsigned char)host_base[i]);
	snprintf(host, sizeof(host), "%s", host_base);

	openlog("tinymdns", LOG_PID | LOG_PERROR, LOG_DAEMON);
	srand((unsigned)time(NULL) ^ (unsigned)getpid());
	signal(SIGPIPE, SIG_IGN);

	sock.fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (sock.fd < 0) {
		syslog(LOG_ERR, "socket: %s", strerror(errno));
		return 1;
	}
	setsockopt(sock.fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
	setsockopt(sock.fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
	setsockopt(sock.fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));
	setsockopt(sock.fd, IPPROTO_IP, IP_MULTICAST_TTL, &mttl, sizeof(mttl));
	setsockopt(sock.fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
	setsockopt(sock.fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
	memset(&any, 0, sizeof(any));
	any.sin_family = AF_INET;
	any.sin_port = htons(MDNS_PORT);
	if (bind(sock.fd, (struct sockaddr *)&any, sizeof(any)) < 0) {
		syslog(LOG_ERR, "bind udp/%d: %s", MDNS_PORT, strerror(errno));
		return 1;
	}
	memset(&group, 0, sizeof(group));
	group.sin_family = AF_INET;
	group.sin_port = htons(MDNS_PORT);
	inet_pton(AF_INET, MDNS_GROUP, &group.sin_addr);

	uloop_init();
	sock.cb = sock_cb;
	uloop_fd_add(&sock, ULOOP_READ);
	step_timer.cb = step_cb;
	if_timer.cb = if_cb;
	if_cb(&if_timer);
	syslog(LOG_INFO, "serving %s.local on %s with %u service(s)", host, ifname, nsvc);

	uloop_run();  /* until SIGTERM / SIGINT */

	if (state == ANNOUNCING || state == RUNNING)
		send_all(0, true);  /* goodbye: TTL 0 */
	uloop_done();
	close(sock.fd);
	return 0;
}
