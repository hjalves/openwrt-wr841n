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
#define STATE_VERSION     1u

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
static const char *cfg_state;      /* optional persistence file */
static unsigned cfg_interval = 60; /* seconds */
static unsigned cfg_capacity = 4320;
static unsigned cfg_save_every;    /* seconds, 0 = only on exit */

/* ---- history ring buffer ---- */
static struct sample *ring;
static unsigned ring_head;  /* index of oldest sample */
static unsigned ring_count;

/* ---- status ---- */
static uint32_t last_attempt, last_success;
static unsigned failures;  /* consecutive */
static char last_error[96];

static void ring_push(const struct sample *s)
{
	ring[(ring_head + ring_count) % cfg_capacity] = *s;
	if (ring_count < cfg_capacity)
		ring_count++;
	else
		ring_head = (ring_head + 1) % cfg_capacity;
}

static const struct sample *ring_at(unsigned i) /* 0 = oldest */
{
	return &ring[(ring_head + i) % cfg_capacity];
}

/* ------------------------------------------------------------------ */
/* Persistence (optional)                                             */
/* ------------------------------------------------------------------ */

static void state_save(void)
{
	char tmp[PATH_MAX];
	uint32_t hdr[3] = { STATE_MAGIC, STATE_VERSION, ring_count };
	unsigned i;
	FILE *f;

	if (!cfg_state)
		return;
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", cfg_state) >= (int)sizeof(tmp))
		return;
	f = fopen(tmp, "wb");
	if (!f) {
		syslog(LOG_WARNING, "cannot write %s: %s", tmp, strerror(errno));
		return;
	}
	fwrite(hdr, sizeof(hdr), 1, f);
	for (i = 0; i < ring_count; i++)
		fwrite(ring_at(i), sizeof(struct sample), 1, f);
	if (fflush(f) != 0 || ferror(f) || fsync(fileno(f)) != 0) {
		fclose(f);
		unlink(tmp);
		syslog(LOG_WARNING, "write to %s failed", tmp);
		return;
	}
	fclose(f);
	if (rename(tmp, cfg_state) != 0) {
		unlink(tmp);
		syslog(LOG_WARNING, "rename to %s failed: %s", cfg_state, strerror(errno));
	}
}

static void state_load(void)
{
	uint32_t hdr[3];
	struct sample s;
	unsigned i;
	FILE *f;

	if (!cfg_state || !(f = fopen(cfg_state, "rb")))
		return;
	if (fread(hdr, sizeof(hdr), 1, f) == 1 &&
	    hdr[0] == STATE_MAGIC && hdr[1] == STATE_VERSION) {
		for (i = 0; i < hdr[2] && fread(&s, sizeof(s), 1, f) == 1; i++)
			ring_push(&s);  /* keeps only the newest cfg_capacity */
		syslog(LOG_INFO, "loaded %u samples from %s", ring_count, cfg_state);
	} else {
		syslog(LOG_WARNING, "ignoring unrecognised state file %s", cfg_state);
	}
	fclose(f);
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
	ring_push(&s);
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
	uloop_timeout_set(t, cfg_interval * 1000);
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
	if (!ring_count)
		return UBUS_STATUS_NO_DATA;
	blob_buf_init(&b, 0);
	add_sample(&b, "sample", ring_at(ring_count - 1));
	return ubus_send_reply(ctx, req, b.head);
}

enum { Q_FROM, Q_TO, Q_LIMIT, __Q_MAX };
static const struct blobmsg_policy query_policy[__Q_MAX] = {
	[Q_FROM]  = { .name = "from",  .type = BLOBMSG_TYPE_INT32 },
	[Q_TO]    = { .name = "to",    .type = BLOBMSG_TYPE_INT32 },
	[Q_LIMIT] = { .name = "limit", .type = BLOBMSG_TYPE_INT32 },
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

static int m_query(struct ubus_context *ctx, struct ubus_object *obj,
                   struct ubus_request_data *req, const char *method,
                   struct blob_attr *msg)
{
	struct blob_attr *tb[__Q_MAX];
	int64_t now = time(NULL), from, to;
	unsigned i, n = 0, limit = QUERY_DEFAULT_MAX;
	bool truncated = false;
	void *arr;

	(void)obj; (void)method;
	blobmsg_parse(query_policy, __Q_MAX, tb, blob_data(msg), blob_len(msg));
	from = abs_time(tb[Q_FROM], 0, now);
	to = abs_time(tb[Q_TO], INT64_MAX, now);
	if (tb[Q_LIMIT]) {
		int32_t l = (int32_t)blobmsg_get_u32(tb[Q_LIMIT]);
		if (l < 1)
			return UBUS_STATUS_INVALID_ARGUMENT;
		limit = l > QUERY_HARD_MAX ? QUERY_HARD_MAX : (unsigned)l;
	}

	blob_buf_init(&b, 0);
	arr = blobmsg_open_array(&b, "samples");
	for (i = 0; i < ring_count; i++) {
		const struct sample *s = ring_at(i);

		if (s->time < from || s->time > to)
			continue;
		if (n == limit) {
			truncated = true;  /* oldest first; continue with from=last.time+1 */
			break;
		}
		add_sample(&b, NULL, s);
		n++;
	}
	blobmsg_close_array(&b, arr);
	blobmsg_add_u32(&b, "count", n);
	blobmsg_add_u8(&b, "truncated", truncated);
	return ubus_send_reply(ctx, req, b.head);
}

static int m_status(struct ubus_context *ctx, struct ubus_object *obj,
                    struct ubus_request_data *req, const char *method,
                    struct blob_attr *msg)
{
	(void)obj; (void)method; (void)msg;
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "source", cfg_host);
	blobmsg_add_u32(&b, "interval", cfg_interval);
	blobmsg_add_u32(&b, "capacity", cfg_capacity);
	blobmsg_add_u32(&b, "count", ring_count);
	if (ring_count) {
		blobmsg_add_u32(&b, "oldest", ring_at(0)->time);
		blobmsg_add_u32(&b, "newest", ring_at(ring_count - 1)->time);
	}
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
		"  -n samples   history size kept in RAM (default 4320, 40 bytes each)\n"
		"  -f file      load history at start and save it on exit\n"
		"  -S seconds   also save the history file this often (default: exit only)\n"
		"  -s socket    ubus socket path\n", prog);
	exit(2);
}

int main(int argc, char **argv)
{
	int opt;

	while ((opt = getopt(argc, argv, "H:P:i:n:f:S:s:h")) != -1) {
		switch (opt) {
		case 'H': cfg_host = optarg; break;
		case 'P': cfg_port = optarg; break;
		case 'i': cfg_interval = parse_uint(optarg, "interval", 5, 86400); break;
		case 'n': cfg_capacity = parse_uint(optarg, "history size", 1, 1000000); break;
		case 'f': cfg_state = optarg; break;
		case 'S': cfg_save_every = parse_uint(optarg, "save interval", 60, 604800); break;
		case 's': cfg_socket = optarg; break;
		default: usage(argv[0]);
		}
	}
	if (optind != argc)
		usage(argv[0]);

	openlog("huawei-trafficd", LOG_PID | LOG_PERROR, LOG_DAEMON);
	ring = calloc(cfg_capacity, sizeof(*ring));
	if (!ring) {
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
	if (cfg_state && cfg_save_every) {
		save_timer.cb = save_timer_cb;
		uloop_timeout_set(&save_timer, cfg_save_every * 1000);
	}
	syslog(LOG_INFO, "sampling %s:%s every %us, keeping %u samples",
	       cfg_host, cfg_port, cfg_interval, cfg_capacity);

	uloop_run();  /* until SIGTERM/SIGINT */

	http_close();
	state_save();
	if (ubus_ever_connected)  /* the context is only valid once connected */
		ubus_shutdown(&conn.ctx);
	uloop_done();
	free(ring);
	return 0;
}
