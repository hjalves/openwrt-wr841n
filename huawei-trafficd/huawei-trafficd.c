/*
 * huawei-trafficd: periodically sample the traffic counters of a Huawei
 * HiLink LTE router and expose the history on ubus as "huawei.traffic".
 *
 * Collection: GET /api/webserver/SesTokInfo (anonymous session cookie), then
 * GET /api/monitoring/traffic-statistics with that cookie. No login needed.
 * Everything is non-blocking inside a single uloop, so ubus calls are never
 * held up by a slow or unreachable Huawei router.
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
#include <sys/stat.h>
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
#define STATE_VERSION     2u          /* v2 adds the tier interval; v1 still loads */

struct sample {
	uint32_t time;          /* Unix time on the OpenWrt router */
	uint32_t uptime;        /* Huawei CurrentConnectTime, seconds */
	uint64_t download;      /* CurrentDownload, bytes this session */
	uint64_t upload;        /* CurrentUpload, bytes this session */
	uint64_t total_download;
	uint64_t total_upload;
};

/* ---- configuration ---- */
static const char *cfg_host = "192.168.0.1";
static const char *cfg_port = "80";
static const char *cfg_socket;     /* ubus socket, NULL = default */
static unsigned cfg_save_every;    /* seconds, 0 = only on exit */

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

/* Keep s in the coarse tier if it starts a new coarse_interval bucket. */
static void coarse_offer(const struct sample *s)
{
	if (!coarse.interval)
		return;
	if (coarse.count &&
	    ring_at(&coarse, coarse.count - 1)->time / coarse.interval == s->time / coarse.interval)
		return;
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
	    (hdr[1] != 1 && hdr[1] != 2) ||
	    (hdr[1] == 2 && fread(&hdr[3], sizeof(uint32_t), 1, f) != 1)) {
		syslog(LOG_WARNING, "ignoring unrecognised state file %s", r->state);
		fclose(f);
		return;
	}
	if (hdr[1] == 2 && hdr[3] != r->interval)
		syslog(LOG_NOTICE, "%s was recorded at %us intervals, now %us",
		       r->state, hdr[3], r->interval);
	for (i = 0; i < hdr[2] && fread(&s, sizeof(s), 1, f) == 1; i++)
		ring_push(r, &s);  /* keeps only the newest r->cap */
	syslog(LOG_INFO, "loaded %u %s samples from %s", r->count, r->name, r->state);
	fclose(f);
}

static void state_load(void)
{
	unsigned i;

	ring_load(&fine);
	if (!coarse.interval)
		return;
	ring_load(&coarse);
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

enum stage { STAGE_TOKEN, STAGE_STATS };

static struct {
	struct uloop_fd fd;
	struct uloop_timeout timeout;
	bool busy, connecting, cookie_was_cached;
	enum stage stage;
	char req[COOKIE_SIZE + 256];
	size_t req_len, sent;
	char buf[HTTP_BUF_SIZE];
	size_t len;
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

static void sample_failed(const char *fmt, ...)
{
	va_list ap;

	http_close();
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
	struct sample s;
	char *body, code[16];
	size_t cl;
	bool have_cl;
	uint64_t v;

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

	memset(&s, 0, sizeof(s));
	if (!xml_u64(body, "CurrentConnectTime", &v) ||
	    !xml_u64(body, "CurrentDownload", &s.download) ||
	    !xml_u64(body, "CurrentUpload", &s.upload))
		FAIL("counters missing in response");
	s.uptime = v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
	xml_u64(body, "TotalDownload", &s.total_download);
	xml_u64(body, "TotalUpload", &s.total_upload);
	s.time = (uint32_t)time(NULL);

	http_close();
	http.busy = false;
	record_sample(&s);
	last_success = s.time;
	if (failures)
		syslog(LOG_NOTICE, "sampling recovered after %u failures", failures);
	failures = 0;
	last_error[0] = '\0';
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
	const char *path = http.stage == STAGE_TOKEN ?
		"/api/webserver/SesTokInfo" : "/api/monitoring/traffic-statistics";
	int n;

	http_close();
	n = snprintf(http.req, sizeof(http.req),
		"GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n%s%s%s\r\n",
		path, cfg_host,
		http.stage == STAGE_STATS ? "Cookie: " : "",
		http.stage == STAGE_STATS ? cookie : "",
		http.stage == STAGE_STATS ? "\r\n" : "");
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
	http.busy = true;
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
	return ubus_send_reply(ctx, req, b.head);
}

static const struct ubus_method methods[] = {
	UBUS_METHOD_NOARG("latest", m_latest),
	UBUS_METHOD("query", m_query, query_policy),
	UBUS_METHOD_NOARG("status", m_status),
};

static struct ubus_object_type obj_type = UBUS_OBJECT_TYPE("huawei.traffic", methods);

static struct ubus_object ubus_obj = {
	.name = "huawei.traffic",
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
		"  -n samples   fine history size (default 4320, 40 bytes each)\n"
		"  -c seconds   coarse tier interval (default 3600, 0 disables it)\n"
		"  -N samples   coarse history size (default 8760, 40 bytes each)\n"
		"  -f file      fine history file: loaded at start, saved on exit\n"
		"  -F file      coarse history file (default: <fine file>.coarse)\n"
		"  -S seconds   also save the history files this often (default: exit only)\n"
		"  -s socket    ubus socket path\n", prog);
	exit(2);
}

int main(int argc, char **argv)
{
	int opt;

	static char coarse_default[PATH_MAX];

	while ((opt = getopt(argc, argv, "H:P:i:n:c:N:f:F:S:s:h")) != -1) {
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
	state_load();

	uloop_init();
	conn.path = cfg_socket;
	conn.cb = ubus_connected;
	ubus_auto_connect(&conn);

	sample_timer.cb = sample_timer_cb;
	uloop_timeout_set(&sample_timer, 1000);
	if ((fine.state || coarse.state) && cfg_save_every) {
		save_timer.cb = save_timer_cb;
		uloop_timeout_set(&save_timer, cfg_save_every * 1000);
	}
	syslog(LOG_INFO, "sampling %s:%s every %us, keeping %u samples; coarse tier: "
	       "%u samples every %us", cfg_host, cfg_port, fine.interval, fine.cap,
	       coarse.interval ? coarse.cap : 0, coarse.interval);

	uloop_run();  /* until SIGTERM/SIGINT */

	http_close();
	state_save();
	if (ubus_ever_connected)  /* the context is only valid once connected */
		ubus_shutdown(&conn.ctx);
	uloop_done();
	free(fine.buf);
	free(coarse.buf);
	return 0;
}
