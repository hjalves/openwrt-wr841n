/*
 * tvremote: control a Samsung Tizen TV (2016 "K" series and similar) like its
 * remote, exposed on ubus as "tv".
 *
 * - Power on: Wake-on-LAN magic packet (UDP broadcast).
 * - Keys and power off: the TV's remote-control WebSocket,
 *   ws://<tv>:8001/api/v2/channels/samsung.remote.control?name=<base64>,
 *   one JSON text frame per key ("ms.remote.control" / "SendRemoteKey").
 *   These TVs accept plain ws:// on port 8001 with no token.
 *
 * The daemon keeps the WebSocket open while the TV is on (so keys go out
 * with no reconnect delay) and tracks the TV's power state:
 *
 *   off ──power_on──▶ waking ──connected──▶ on ──power_off──▶ turning_off ──▶ off
 *
 * After KEY_POWER the TV keeps its network up for about 17 s; "turning_off"
 * covers that window, so the daemon neither mistakes the dying TV for "on"
 * nor sends another KEY_POWER that would switch it back on. While "off" it
 * probes the TV every so often (default 30 s), so switching it on with its
 * own remote is noticed too.
 *
 * Only the router's existing shared libraries are used: libubus, libubox and
 * uClibc. No TLS is needed.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <libubus.h>
#include <libubox/blobmsg.h>
#include <libubox/usock.h>
#include <libubox/utils.h>

#define RX_CAP            8192   /* TV messages are a few hundred bytes */
#define TX_CAP            512
#define CONNECT_TIMEOUT   4000   /* ms: TCP connect + HTTP upgrade */
#define CHANNEL_TIMEOUT   30000  /* ms: upgraded, waiting for ms.channel.connect
                                    (long enough to answer an "Allow" prompt) */
#define WAKE_PROBE_MS     2000   /* probe interval while waking */
#define RETRY_MS          1000   /* reconnect delay after a drop while on */
#define QUEUE_MAX         8      /* keys pressed while the TV wakes up */
#define KEY_MAX           40

/* ---- configuration ---- */
static const char *cfg_host;
static const char *cfg_port = "8001";
static const char *cfg_name = "OpenWrt";
static const char *cfg_bcast = "255.255.255.255";
static const char *cfg_socket;
static uint8_t cfg_mac[6];
static bool have_mac;
static unsigned cfg_poll = 30;      /* s, probe while off; 0 = only on request */
static unsigned cfg_wake = 40;      /* s, give up waking after this */
static unsigned cfg_offwin = 25;    /* s, the TV's shutdown window */

/* ---- power state ---- */
enum tv_state { S_OFF, S_WAKING, S_ON, S_TURNING_OFF };
static const char *const state_names[] = { "off", "waking", "on", "turning_off" };
static enum tv_state state = S_OFF;
static time_t state_since;
static bool pending_on, pending_off;   /* requested during a transition */
static time_t wake_deadline;

/* ---- connection ---- */
enum conn_phase { C_IDLE, C_CONNECTING, C_HANDSHAKE, C_UPGRADED, C_READY };
static enum conn_phase phase = C_IDLE;
static struct uloop_fd sock = { .fd = -1 };
static struct uloop_timeout conn_timer, poll_timer, off_timer;
static uint8_t rx[RX_CAP + 1];
static size_t rx_len;
static time_t ready_since;

static char queue[QUEUE_MAX][KEY_MAX + 8];  /* "Cmd KEY_..." */
static unsigned nqueued;

static char last_error[160];
static time_t last_error_time;
static unsigned keys_sent, wol_sent;

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(last_error, sizeof(last_error), fmt, ap);
	va_end(ap);
	last_error_time = time(NULL);
	syslog(LOG_WARNING, "%s", last_error);
}

static void set_state(enum tv_state s)
{
	if (s == state)
		return;
	syslog(LOG_INFO, "TV %s -> %s", state_names[state], state_names[s]);
	state = s;
	state_since = time(NULL);
}

/* ------------------------------------------------------------------ */
/* Wake-on-LAN */

static bool send_wol(void)
{
	uint8_t pkt[102];
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(9) };
	int fd, on = 1, i;
	ssize_t n;

	memset(pkt, 0xff, 6);
	for (i = 0; i < 16; i++)
		memcpy(pkt + 6 + i * 6, cfg_mac, 6);
	inet_pton(AF_INET, cfg_bcast, &a.sin_addr);  /* validated at start */
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		set_error("wake-on-LAN: socket: %s", strerror(errno));
		return false;
	}
	setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
	n = sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&a, sizeof(a));
	close(fd);
	if (n != (ssize_t)sizeof(pkt)) {
		set_error("wake-on-LAN to %s: %s", cfg_bcast, n < 0 ? strerror(errno) : "short send");
		return false;
	}
	wol_sent++;
	return true;
}

/* ------------------------------------------------------------------ */
/* WebSocket client */

static void schedule_poll(void);
static void conn_ready(void);

static void conn_close(const char *why)
{
	bool was_ready = phase == C_READY, was_connected = phase >= C_HANDSHAKE;

	if (phase == C_IDLE)
		return;
	uloop_timeout_cancel(&conn_timer);
	uloop_fd_delete(&sock);
	close(sock.fd);
	sock.fd = -1;
	phase = C_IDLE;
	rx_len = 0;

	switch (state) {
	case S_ON:
		if (was_ready && time(NULL) - ready_since >= 10) {
			/* Dropped after working for a while: maybe the TV is still on
			 * and just closed the socket. Check again right away; keys
			 * pressed meanwhile are queued. */
			if (why)
				syslog(LOG_INFO, "connection lost (%s); reconnecting", why);
			uloop_timeout_set(&poll_timer, RETRY_MS);
			return;
		}
		set_state(S_OFF);  /* a reconnect attempt failed: it's off */
		nqueued = 0;
		break;
	case S_WAKING:
		/* A sleeping TV just doesn't answer; only report odd failures. */
		if (why && was_connected)
			set_error("while waking: %s", why);
		break;
	case S_OFF:
	case S_TURNING_OFF:
		break;
	}
	schedule_poll();
}

static bool ws_send(uint8_t opcode, const void *data, size_t len)
{
	uint8_t f[TX_CAP], *m;
	const uint8_t *d = data;
	size_t h, i;
	uint32_t r = (uint32_t)random();

	if (len > TX_CAP - 8)
		return false;
	f[0] = 0x80 | opcode;               /* FIN */
	if (len < 126) {
		f[1] = 0x80 | len;              /* client frames are masked */
		h = 2;
	} else {
		f[1] = 0x80 | 126;
		f[2] = len >> 8;
		f[3] = len & 0xff;
		h = 4;
	}
	m = f + h;
	memcpy(m, &r, 4);
	for (i = 0; i < len; i++)
		f[h + 4 + i] = d[i] ^ m[i & 3];
	len += h + 4;
	if (send(sock.fd, f, len, MSG_NOSIGNAL) != (ssize_t)len) {
		set_error("send to TV: %s", strerror(errno));
		conn_close(NULL);
		return false;
	}
	return true;
}

static bool send_key(const char *cmd, const char *key)
{
	char msg[256];
	int n = snprintf(msg, sizeof(msg),
		"{\"method\":\"ms.remote.control\",\"params\":{\"Cmd\":\"%s\",\"DataOfCmd\":\"%s\","
		"\"Option\":\"false\",\"TypeOfRemote\":\"SendRemoteKey\"}}", cmd, key);

	if (n < 0 || n >= (int)sizeof(msg) || !ws_send(0x1, msg, n))
		return false;
	keys_sent++;
	return true;
}

static void send_handshake(void)
{
	char name64[96], key64[32], req[512];
	uint8_t nonce[16];
	unsigned i;
	int n;

	for (i = 0; i < sizeof(nonce); i++)
		nonce[i] = (uint8_t)random();
	b64_encode(cfg_name, strlen(cfg_name), name64, sizeof(name64));
	b64_encode(nonce, sizeof(nonce), key64, sizeof(key64));
	/* The base64 name goes in raw, as other Samsung remote clients send it. */
	n = snprintf(req, sizeof(req),
		"GET /api/v2/channels/samsung.remote.control?name=%s HTTP/1.1\r\n"
		"Host: %s:%s\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: %s\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n", name64, cfg_host, cfg_port, key64);
	if (send(sock.fd, req, n, MSG_NOSIGNAL) != n) {
		conn_close("send failed");
		return;
	}
	phase = C_HANDSHAKE;
}

/* Copies the value of "event":"..." from a TV message. */
static bool json_event(const char *s, char *out, size_t size)
{
	const char *p = strstr(s, "\"event\":\""), *e;

	if (!p)
		return false;
	p += 9;
	e = strchr(p, '"');
	if (!e || (size_t)(e - p) >= size)
		return false;
	memcpy(out, p, e - p);
	out[e - p] = 0;
	return true;
}

static void on_text(char *s)
{
	char ev[64];

	if (!json_event(s, ev, sizeof(ev)))
		return;
	if (!strcmp(ev, "ms.channel.connect")) {
		if (phase == C_UPGRADED)
			conn_ready();
	} else if (!strcmp(ev, "ms.channel.unauthorized") || !strcmp(ev, "ms.channel.timeOut")) {
		set_error("TV refused the remote (%s): allow \"%s\" on the TV", ev, cfg_name);
		conn_close(NULL);
	}
}

/* Parses as many complete frames (or the HTTP response) as rx holds.
 * Returns false once the connection has been closed. */
static bool process_rx(void)
{
	for (;;) {
		if (phase == C_HANDSHAKE) {
			char *end;

			rx[rx_len] = 0;
			end = strstr((char *)rx, "\r\n\r\n");
			if (!end)
				return true;
			if (strncmp((char *)rx, "HTTP/1.1 101", 12)) {
				char *eol = strchr((char *)rx, '\r');

				*eol = 0;
				set_error("TV rejected the WebSocket: %.80s", (char *)rx);
				conn_close(NULL);
				return false;
			}
			/* Sec-WebSocket-Accept isn't verified: it would need SHA-1,
			 * and it protects nothing on a LAN connection to a known IP. */
			end += 4;
			rx_len -= end - (char *)rx;
			memmove(rx, end, rx_len);
			phase = C_UPGRADED;
			uloop_timeout_set(&conn_timer, CHANNEL_TIMEOUT);
			continue;
		}

		/* A frame: 2-byte header, extended length, optional mask, payload. */
		uint8_t op, *p, *mask = NULL;
		uint64_t len;
		size_t h = 2, i;

		if (rx_len < 2)
			return true;
		op = rx[0] & 0x0f;
		len = rx[1] & 0x7f;
		if (len == 126)
			h = 4;
		else if (len == 127)
			h = 10;
		if (rx[1] & 0x80)
			h += 4;
		if (rx_len < h)
			return true;
		if (len == 126)
			len = (uint64_t)rx[2] << 8 | rx[3];
		else if (len == 127)
			for (len = 0, i = 2; i < 10; i++)
				len = len << 8 | rx[i];
		if (len > RX_CAP - h) {
			conn_close("message from TV too large");
			return false;
		}
		if (rx_len < h + len)
			return true;
		p = rx + h;
		if (rx[1] & 0x80) {             /* servers shouldn't mask, but accept it */
			mask = p - 4;
			for (i = 0; i < len; i++)
				p[i] ^= mask[i & 3];
		}

		switch (op) {
		case 0x1: {                     /* text */
			uint8_t saved = p[len];

			p[len] = 0;
			on_text((char *)p);
			if (phase == C_IDLE)
				return false;
			p[len] = saved;
			break;
		}
		case 0x8:                       /* close */
			conn_close("closed by the TV");
			return false;
		case 0x9:                       /* ping */
			if (!ws_send(0xA, p, len))
				return false;
			break;
		default:                        /* pong, binary, continuation: unused */
			break;
		}
		rx_len -= h + len;
		memmove(rx, rx + h + len, rx_len);
	}
}

static void sock_cb(struct uloop_fd *u, unsigned int events)
{
	if (phase == C_CONNECTING && (events & ULOOP_WRITE || u->error)) {
		int err = 0, on = 1, v;
		socklen_t l = sizeof(err);

		getsockopt(u->fd, SOL_SOCKET, SO_ERROR, &err, &l);
		if (err) {
			conn_close(strerror(err));
			return;
		}
		/* Notice a TV that vanished (standby) within about 25 s. */
		setsockopt(u->fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
		v = 10; setsockopt(u->fd, IPPROTO_TCP, TCP_KEEPIDLE, &v, sizeof(v));
		v = 5;  setsockopt(u->fd, IPPROTO_TCP, TCP_KEEPINTVL, &v, sizeof(v));
		v = 3;  setsockopt(u->fd, IPPROTO_TCP, TCP_KEEPCNT, &v, sizeof(v));
		uloop_fd_delete(u);
		uloop_fd_add(u, ULOOP_READ);
		send_handshake();
		return;
	}
	if (u->error) {  /* uloop has already dropped the fd */
		int err = 0;
		socklen_t l = sizeof(err);

		getsockopt(u->fd, SOL_SOCKET, SO_ERROR, &err, &l);
		conn_close(err ? strerror(err) : "connection error");
		return;
	}
	if (!(events & ULOOP_READ) && !u->eof)
		return;
	for (;;) {
		ssize_t n;

		if (rx_len == RX_CAP) {
			conn_close("message from TV too large");
			return;
		}
		n = recv(u->fd, rx + rx_len, RX_CAP - rx_len, 0);
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			conn_close(n == 0 ? "closed by the TV" : strerror(errno));
			return;
		}
		rx_len += n;
		if (!process_rx())
			return;
	}
}

static void conn_timeout_cb(struct uloop_timeout *t)
{
	(void)t;
	if (phase == C_UPGRADED)
		set_error("TV accepted the WebSocket but never opened the remote channel");
	conn_close(phase == C_CONNECTING ? "connect timeout" : "no answer from the TV");
}

static void conn_start(void)
{
	if (phase != C_IDLE)
		return;
	sock.fd = usock(USOCK_TCP | USOCK_NONBLOCK | USOCK_NUMERIC | USOCK_IPV4ONLY, cfg_host, cfg_port);
	if (sock.fd < 0) {
		set_error("connect to %s:%s: %s", cfg_host, cfg_port, strerror(errno));
		schedule_poll();
		return;
	}
	fcntl(sock.fd, F_SETFD, FD_CLOEXEC);
	sock.cb = sock_cb;
	phase = C_CONNECTING;
	rx_len = 0;
	uloop_fd_add(&sock, ULOOP_WRITE);
	uloop_timeout_set(&conn_timer, CONNECT_TIMEOUT);
}

static bool power_on_now(void);

static void conn_ready(void)
{
	unsigned i;

	uloop_timeout_cancel(&conn_timer);
	phase = C_READY;
	ready_since = time(NULL);
	if (state == S_TURNING_OFF) {
		/* Shouldn't happen (no probing in the window); don't trust it. */
		conn_close(NULL);
		return;
	}
	set_state(S_ON);
	uloop_timeout_cancel(&poll_timer);
	if (pending_off) {
		pending_off = false;
		nqueued = 0;
		if (send_key("Click", "KEY_POWER")) {
			set_state(S_TURNING_OFF);  /* socket stays open: see power_off_now */
			uloop_timeout_set(&off_timer, cfg_offwin * 1000);
		}
		return;
	}
	for (i = 0; i < nqueued && phase == C_READY; i++) {
		char *sp = strchr(queue[i], ' ');

		*sp = 0;
		send_key(queue[i], sp + 1);
	}
	nqueued = 0;
}

/* ------------------------------------------------------------------ */
/* Probing and power transitions */

static void schedule_poll(void)
{
	if (phase != C_IDLE)
		return;
	switch (state) {
	case S_WAKING:
		if (time(NULL) >= wake_deadline) {
			set_error("TV didn't wake within %u s", cfg_wake);
			set_state(S_OFF);
			nqueued = 0;
			pending_off = false;
			schedule_poll();
			return;
		}
		uloop_timeout_set(&poll_timer, WAKE_PROBE_MS);
		break;
	case S_OFF:
		if (cfg_poll)
			uloop_timeout_set(&poll_timer, cfg_poll * 1000);
		else
			uloop_timeout_cancel(&poll_timer);
		break;
	case S_ON:                          /* reconnect handled in conn_close */
	case S_TURNING_OFF:                 /* off_timer resumes probing */
		break;
	}
}

static void poll_cb(struct uloop_timeout *t)
{
	(void)t;
	if (state != S_TURNING_OFF)
		conn_start();
}

static bool power_on_now(void)
{
	bool ok = send_wol();

	if (state != S_WAKING)
		wake_deadline = time(NULL) + cfg_wake;
	set_state(S_WAKING);
	if (phase == C_IDLE) {
		uloop_timeout_cancel(&poll_timer);
		/* Give the TV a moment: probing a sleeping TV only times out. */
		uloop_timeout_set(&poll_timer, WAKE_PROBE_MS);
	}
	return ok;
}

static void off_cb(struct uloop_timeout *t)
{
	(void)t;
	conn_close(NULL);  /* if the TV hasn't closed it already */
	set_state(S_OFF);
	if (pending_on) {
		pending_on = false;
		power_on_now();
		return;
	}
	conn_start();  /* confirm it really went off */
}

static void power_off_now(void)
{
	nqueued = 0;
	pending_on = false;
	if (send_key("Click", "KEY_POWER")) {
		/* Keep the socket open until the window ends (or the TV closes it):
		 * closing at once can send a TCP reset, and the TV then drops the
		 * key it hasn't read yet. */
		set_state(S_TURNING_OFF);
		uloop_timeout_cancel(&poll_timer);
		uloop_timeout_set(&off_timer, cfg_offwin * 1000);
	} else {
		pending_off = true;  /* send failed; reconnecting, retry then */
	}
}

/* ------------------------------------------------------------------ */
/* ubus */

static struct blob_buf b;

static void reply_state(struct ubus_context *ctx, struct ubus_request_data *req, const char *extra)
{
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "state", state_names[state]);
	if (pending_on)
		blobmsg_add_string(&b, "then", "on");
	else if (pending_off)
		blobmsg_add_string(&b, "then", "off");
	if (extra)
		blobmsg_add_u8(&b, extra, 1);
	ubus_send_reply(ctx, req, b.head);
}

static int m_status(struct ubus_context *ctx, struct ubus_object *obj,
                    struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	char mac[18];
	time_t now = time(NULL);

	(void)obj; (void)method; (void)msg;
	snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
	         cfg_mac[0], cfg_mac[1], cfg_mac[2], cfg_mac[3], cfg_mac[4], cfg_mac[5]);
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "state", state_names[state]);
	blobmsg_add_u32(&b, "since", (uint32_t)(now - state_since));
	blobmsg_add_u8(&b, "connected", phase == C_READY);
	if (pending_on)
		blobmsg_add_string(&b, "then", "on");
	else if (pending_off)
		blobmsg_add_string(&b, "then", "off");
	blobmsg_add_string(&b, "host", cfg_host);
	blobmsg_add_string(&b, "mac", mac);
	blobmsg_add_string(&b, "name", cfg_name);
	blobmsg_add_u32(&b, "keys_sent", keys_sent);
	blobmsg_add_u32(&b, "wol_sent", wol_sent);
	if (last_error[0]) {
		blobmsg_add_string(&b, "last_error", last_error);
		blobmsg_add_u32(&b, "last_error_age", (uint32_t)(now - last_error_time));
	}
	return ubus_send_reply(ctx, req, b.head);
}

static int m_power_on(struct ubus_context *ctx, struct ubus_object *obj,
                      struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj; (void)method; (void)msg;
	pending_off = false;
	switch (state) {
	case S_ON:
		break;
	case S_TURNING_OFF:
		pending_on = true;  /* WoL once the TV has finished switching off */
		break;
	case S_OFF:
	case S_WAKING:
		if (!power_on_now())
			return UBUS_STATUS_UNKNOWN_ERROR;
		break;
	}
	reply_state(ctx, req, NULL);
	return 0;
}

static int m_power_off(struct ubus_context *ctx, struct ubus_object *obj,
                       struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj; (void)method; (void)msg;
	switch (state) {
	case S_ON:
		if (phase == C_READY)
			power_off_now();
		else
			pending_off = true;     /* reconnecting */
		break;
	case S_WAKING:
		pending_off = true;         /* KEY_POWER once it's up */
		nqueued = 0;
		break;
	case S_TURNING_OFF:
		pending_on = false;
		break;
	case S_OFF:
		break;
	}
	reply_state(ctx, req, NULL);
	return 0;
}

enum { K_KEY, K_CMD, __K_MAX };
static const struct blobmsg_policy key_policy[__K_MAX] = {
	[K_KEY] = { .name = "key", .type = BLOBMSG_TYPE_STRING },
	[K_CMD] = { .name = "cmd", .type = BLOBMSG_TYPE_STRING },
};

/* Any KEY_ name the TV might know: KEY_ followed by A-Z, 0-9 or _. */
static bool valid_key(const char *k)
{
	size_t n = strlen(k), i;

	if (n < 5 || n > KEY_MAX || strncmp(k, "KEY_", 4))
		return false;
	for (i = 4; i < n; i++)
		if (!isupper((unsigned char)k[i]) && !isdigit((unsigned char)k[i]) && k[i] != '_')
			return false;
	return true;
}

static int m_key(struct ubus_context *ctx, struct ubus_object *obj,
                 struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__K_MAX];
	const char *key, *cmd = "Click";

	(void)obj; (void)method;
	blobmsg_parse(key_policy, __K_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[K_KEY] || !valid_key(key = blobmsg_get_string(tb[K_KEY])))
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (tb[K_CMD]) {
		cmd = blobmsg_get_string(tb[K_CMD]);
		if (strcmp(cmd, "Click") && strcmp(cmd, "Press") && strcmp(cmd, "Release"))
			return UBUS_STATUS_INVALID_ARGUMENT;
	}
	/* KEY_POWER goes through the state machine, so state stays right. */
	if (!strcmp(key, "KEY_POWER") || !strcmp(key, "KEY_POWEROFF")) {
		if (!strcmp(cmd, "Release"))
			return UBUS_STATUS_INVALID_ARGUMENT;
		return state == S_OFF ? m_power_on(ctx, obj, req, method, msg)
		                      : m_power_off(ctx, obj, req, method, msg);
	}

	if (state == S_ON && phase == C_READY) {
		if (!send_key(cmd, key))
			return UBUS_STATUS_UNKNOWN_ERROR;
		reply_state(ctx, req, "sent");
		return 0;
	}
	if (state == S_WAKING || state == S_ON) {   /* on its way: send when connected */
		if (nqueued == QUEUE_MAX)
			return UBUS_STATUS_NO_DATA;
		snprintf(queue[nqueued++], sizeof(queue[0]), "%s %s", cmd, key);
		reply_state(ctx, req, "queued");
		return 0;
	}
	return UBUS_STATUS_NO_DATA;  /* off or switching off */
}

static const struct ubus_method methods[] = {
	UBUS_METHOD_NOARG("status", m_status),
	UBUS_METHOD_NOARG("power_on", m_power_on),
	UBUS_METHOD_NOARG("power_off", m_power_off),
	UBUS_METHOD("key", m_key, key_policy),
};

static struct ubus_object_type obj_type = UBUS_OBJECT_TYPE("tv", methods);

static struct ubus_object ubus_obj = {
	.name = "tv",
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

static struct ubus_auto_conn uconn;

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

static bool parse_mac(const char *s, uint8_t *mac)
{
	unsigned v[6], i;
	char c;

	if (sscanf(s, "%2x%*1[:-]%2x%*1[:-]%2x%*1[:-]%2x%*1[:-]%2x%*1[:-]%2x%c",
	           &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &c) != 6)
		return false;
	for (i = 0; i < 6; i++)
		mac[i] = v[i];
	return true;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s -H address -m mac [options]\n"
		"  -H address   the TV's IPv4 address (give it a fixed DHCP lease)\n"
		"  -m mac       the TV's MAC address, for Wake-on-LAN\n"
		"  -b address   broadcast address for Wake-on-LAN (default 255.255.255.255)\n"
		"  -p port      the TV's remote-control port (default 8001)\n"
		"  -n name      name shown to the TV for this remote (default OpenWrt)\n"
		"  -P seconds   probe the TV this often while off (default 30, 0 = never)\n"
		"  -W seconds   give up waking the TV after this (default 40)\n"
		"  -O seconds   the TV's shutdown window after KEY_POWER (default 25)\n"
		"  -s socket    ubus socket path\n", prog);
	exit(2);
}

int main(int argc, char **argv)
{
	struct in_addr tmp;
	int opt;
	unsigned seed;
	FILE *f;

	while ((opt = getopt(argc, argv, "H:m:b:p:n:P:W:O:s:h")) != -1) {
		switch (opt) {
		case 'H': cfg_host = optarg; break;
		case 'm':
			if (!parse_mac(optarg, cfg_mac)) {
				fprintf(stderr, "invalid MAC address: %s\n", optarg);
				return 2;
			}
			have_mac = true;
			break;
		case 'b': cfg_bcast = optarg; break;
		case 'p': cfg_port = optarg; parse_uint(optarg, "port", 1, 65535); break;
		case 'n':
			cfg_name = optarg;
			if (!*optarg || strlen(optarg) > 48) {
				fprintf(stderr, "name must be 1-48 characters\n");
				return 2;
			}
			break;
		case 'P': cfg_poll = parse_uint(optarg, "probe interval", 0, 86400); break;
		case 'W': cfg_wake = parse_uint(optarg, "wake timeout", 5, 600); break;
		case 'O': cfg_offwin = parse_uint(optarg, "shutdown window", 1, 120); break;
		case 's': cfg_socket = optarg; break;
		default: usage(argv[0]);
		}
	}
	if (optind != argc || !cfg_host || !have_mac)
		usage(argv[0]);
	if (inet_pton(AF_INET, cfg_host, &tmp) != 1) {
		fprintf(stderr, "TV address must be an IPv4 address: %s\n", cfg_host);
		return 2;
	}
	if (inet_pton(AF_INET, cfg_bcast, &tmp) != 1) {
		fprintf(stderr, "broadcast address must be an IPv4 address: %s\n", cfg_bcast);
		return 2;
	}

	/* Masks and the WebSocket nonce only need to be unpredictable-ish. */
	seed = (unsigned)time(NULL) ^ ((unsigned)getpid() << 16);
	if ((f = fopen("/dev/urandom", "rb"))) {
		if (fread(&seed, sizeof(seed), 1, f) != 1)
			seed ^= (unsigned)clock();
		fclose(f);
	}
	srandom(seed);

	openlog("tvremote", LOG_PID | LOG_PERROR, LOG_DAEMON);
	signal(SIGPIPE, SIG_IGN);
	state_since = time(NULL);

	uloop_init();
	conn_timer.cb = conn_timeout_cb;
	poll_timer.cb = poll_cb;
	off_timer.cb = off_cb;
	uconn.path = cfg_socket;
	uconn.cb = ubus_connected;
	ubus_auto_connect(&uconn);
	syslog(LOG_INFO, "controlling the TV at %s:%s; probing every %us while off",
	       cfg_host, cfg_port, cfg_poll);
	conn_start();  /* find out whether it's on */

	uloop_run();  /* until SIGTERM/SIGINT */

	if (phase != C_IDLE) {
		uloop_fd_delete(&sock);
		close(sock.fd);
	}
	if (ubus_ever_connected)
		ubus_shutdown(&uconn.ctx);
	uloop_done();
	return 0;
}
