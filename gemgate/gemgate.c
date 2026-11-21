/*
 * gemgate: a Gemini-to-HTTP gateway, as a CGI program for uhttpd.
 *
 *   GET /cgi-bin/gemini?url=<percent-encoded gemini:// URL>[&trust=<sha256 hex>]
 *
 * It makes one Gemini request (TLS 1.2 with SNI, port 1965 unless the URL says
 * otherwise) and passes the answer on:
 *   - Gemini's status and meta go into HTTP headers (X-Gemini-Status, and
 *     X-Gemini-Meta percent-encoded like encodeURIComponent); the HTTP status
 *     is 200 whenever the Gemini server answered, whatever its status;
 *   - for 2x answers the body is streamed through unbuffered, with the meta as
 *     Content-Type;
 *   - gateway failures (bad URL, DNS, connection, TLS, certificate change...)
 *     come back as HTTP 4xx/5xx with X-Gateway-Error naming the problem.
 *
 * Certificates: trust on first use, as most Gemini clients do. The SHA-256 of
 * a host's certificate is stored in KNOWN_HOSTS the first time it's seen; a
 * different certificate later is refused (X-Gateway-Error: cert-changed) unless
 * the request carries trust=<the new certificate's fingerprint>, which replaces
 * the stored one. CA signatures, names and dates aren't checked.
 *
 * Safety: only gemini:// URLs, no user info, at most 1024 bytes; hosts that
 * resolve to loopback, private, link-local, CGNAT or multicast addresses are
 * refused, so the gateway can't be used to reach the LAN. Answers carry
 * "Content-Security-Policy: sandbox", so a remote HTML file opened through the
 * gateway can't run scripts on the router's site.
 *
 * TLS is BearSSL, linked statically; nothing else beyond uClibc.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "bearssl.h"

#ifndef KNOWN_DIR
#define KNOWN_DIR        "/srv/gemgate"
#endif
#define KNOWN_HOSTS      KNOWN_DIR "/known_hosts"
#define URL_MAX          1024
#define HOST_MAX         255
#define CONNECT_TIMEOUT  10    /* s */
#define IO_TIMEOUT       20    /* s, per read or write */
#define HEADER_MAX       (2 + 1 + 1024 + 2)

static int allow_private;      /* tests only: GEMGATE_ALLOW_PRIVATE=1 */

/* ------------------------------------------------------------------ */
/* Output helpers                                                     */

static void common_headers(void)
{
	fputs("Access-Control-Allow-Origin: *\r\n"
	      "Access-Control-Expose-Headers: X-Gemini-Status, X-Gemini-Meta, X-Gemini-Cert, "
	      "X-Gemini-Cert-Status, X-Gemini-Cert-Known, X-Gateway-Error, X-Gateway-Detail\r\n"
	      "Cache-Control: no-store\r\n"
	      "Content-Security-Policy: sandbox\r\n"
	      "X-Content-Type-Options: nosniff\r\n", stdout);
}

/* A gateway failure: HTTP status, short code and a human-readable message. */
static void fail(int http, const char *code, const char *extra_hdrs, const char *fmt, ...)
	__attribute__((format(printf, 4, 5), noreturn));
static void fail(int http, const char *code, const char *extra_hdrs, const char *fmt, ...)
{
	static const struct { int n; const char *t; } names[] = {
		{400, "Bad Request"}, {403, "Forbidden"}, {405, "Method Not Allowed"},
		{502, "Bad Gateway"}, {504, "Gateway Timeout"} };
	const char *t = "Error";
	va_list ap;
	unsigned i;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (names[i].n == http)
			t = names[i].t;
	printf("Status: %d %s\r\n", http, t);
	common_headers();
	printf("X-Gateway-Error: %s\r\n", code);
	if (extra_hdrs)
		fputs(extra_hdrs, stdout);
	fputs("Content-Type: text/plain; charset=utf-8\r\n\r\n", stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
	exit(0);
}

/* Percent-encode like JavaScript's encodeURIComponent. */
static void put_encoded(const char *s)
{
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (isalnum(c) || strchr("-_.!~*'()", c))
			putchar(c);
		else
			printf("%%%02X", c);
	}
}

static void hex(const unsigned char *b, size_t n, char *out)
{
	size_t i;

	for (i = 0; i < n; i++)
		sprintf(out + 2 * i, "%02x", b[i]);
}

/* ------------------------------------------------------------------ */
/* Query string                                                       */

static int unhex(int c)
{
	return isdigit(c) ? c - '0' : isxdigit(c) ? (tolower(c) - 'a' + 10) : -1;
}

/* Value of name=... in the query string, percent-decoded ('+' stays '+':
 * clients encode with encodeURIComponent). NULL if absent or malformed. */
static char *query_param(const char *qs, const char *name)
{
	size_t nl = strlen(name);
	const char *p = qs;

	while (p && *p) {
		const char *amp = strchr(p, '&'), *end = amp ? amp : p + strlen(p);

		if ((size_t)(end - p) > nl && !strncmp(p, name, nl) && p[nl] == '=') {
			const char *v = p + nl + 1;
			char *out = malloc(end - v + 1), *o = out;

			if (!out)
				return NULL;
			while (v < end) {
				if (*v == '%') {
					int a, b;

					if (end - v < 3 || (a = unhex(v[1])) < 0 || (b = unhex(v[2])) < 0 || (!a && !b)) {
						free(out);
						return NULL;
					}
					*o++ = (char)(a << 4 | b);
					v += 3;
				} else {
					*o++ = *v++;
				}
			}
			*o = 0;
			return out;
		}
		p = amp ? amp + 1 : NULL;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* URL                                                                */

struct gurl {
	char request[URL_MAX + 1];   /* what's sent: the URL without fragment */
	char host[HOST_MAX + 1];     /* lower case */
	char port[6];
};

static void parse_url(const char *in, struct gurl *u)
{
	const char *h, *p, *hend, *frag;
	size_t n, hl;

	frag = strchr(in, '#');
	n = frag ? (size_t)(frag - in) : strlen(in);
	if (n > URL_MAX)
		fail(400, "bad-url", NULL, "The URL is longer than 1024 bytes.");
	if (n < 10 || strncasecmp(in, "gemini://", 9))
		fail(400, "bad-url", NULL, "Only gemini:// URLs can be fetched.");
	for (p = in; p < in + n; p++)
		if ((unsigned char)*p <= 0x20 || *p == 0x7f)
			fail(400, "bad-url", NULL, "The URL contains spaces or control characters.");
	memcpy(u->request, in, n);
	u->request[n] = 0;
	memcpy(u->request, "gemini://", 9);   /* scheme in lower case */

	h = u->request + 9;
	hend = h + strcspn(h, "/?");
	if (memchr(h, '@', hend - h))
		fail(400, "bad-url", NULL, "URLs with user information aren't allowed.");
	if (*h == '[')
		fail(400, "bad-url", NULL, "IPv6 addresses aren't supported (the router has no IPv6).");
	p = memchr(h, ':', hend - h);
	hl = (p ? p : hend) - h;
	if (!hl || hl > HOST_MAX)
		fail(400, "bad-url", NULL, "The URL has no valid host name.");
	for (n = 0; n < hl; n++) {
		unsigned char c = (unsigned char)h[n];

		if (!isalnum(c) && c != '-' && c != '.')
			fail(400, "bad-url", NULL, "Host names must be plain ASCII (internationalised names aren't supported).");
		u->host[n] = (char)tolower(c);
	}
	u->host[hl] = 0;
	strcpy(u->port, "1965");
	if (p) {
		size_t pl = hend - p - 1;
		long v;
		char *e;

		if (!pl || pl > 5)
			fail(400, "bad-url", NULL, "The URL has an invalid port.");
		memcpy(u->port, p + 1, pl);
		u->port[pl] = 0;
		v = strtol(u->port, &e, 10);
		if (*e || v < 1 || v > 65535)
			fail(400, "bad-url", NULL, "The URL has an invalid port.");
	}
}

/* ------------------------------------------------------------------ */
/* Network                                                            */

static int private_addr(const struct in_addr *a)
{
	uint32_t ip = ntohl(a->s_addr);

	return (ip >> 24) == 0 || (ip >> 24) == 10 || (ip >> 24) == 127 ||
	       (ip >> 16) == 0xa9fe ||                 /* 169.254/16 */
	       (ip >> 20) == 0xac1 ||                  /* 172.16/12 */
	       (ip >> 16) == 0xc0a8 ||                 /* 192.168/16 */
	       (ip >> 22) == (100u << 2 | 1) ||        /* 100.64/10 */
	       (ip >> 28) >= 0xe;                      /* multicast, reserved, broadcast */
}

static int connect_host(const struct gurl *u)
{
	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res, *r;
	struct timeval tv = { IO_TIMEOUT, 0 };
	int fd = -1, err, blocked = 0, timed_out = 0;

	if ((err = getaddrinfo(u->host, u->port, &hints, &res)))
		fail(502, "dns", NULL, "Couldn't resolve %s: %s.", u->host, gai_strerror(err));
	for (r = res; r && fd < 0; r = r->ai_next) {
		struct pollfd pfd;
		socklen_t l = sizeof(err);

		if (!allow_private && private_addr(&((struct sockaddr_in *)r->ai_addr)->sin_addr)) {
			blocked = 1;
			continue;
		}
		fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0)
			continue;
		fcntl(fd, F_SETFL, O_NONBLOCK);
		if (connect(fd, r->ai_addr, r->ai_addrlen) < 0 && errno != EINPROGRESS) {
			close(fd);
			fd = -1;
			continue;
		}
		pfd.fd = fd;
		pfd.events = POLLOUT;
		if (poll(&pfd, 1, CONNECT_TIMEOUT * 1000) != 1) {
			timed_out = 1;
			close(fd);
			fd = -1;
			continue;
		}
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) < 0 || err) {
			close(fd);
			fd = -1;
			continue;
		}
		fcntl(fd, F_SETFL, 0);
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	}
	freeaddrinfo(res);
	if (fd < 0 && blocked)
		fail(403, "blocked-address", NULL, "%s resolves to a local or private address, which the gateway doesn't fetch.", u->host);
	if (fd < 0 && timed_out)
		fail(504, "timeout", NULL, "%s:%s didn't accept a connection within %d s.", u->host, u->port, CONNECT_TIMEOUT);
	if (fd < 0)
		fail(502, "connect", NULL, "Couldn't connect to %s:%s.", u->host, u->port);
	return fd;
}

static int timed_out;

static int sock_read(void *ctx, unsigned char *buf, size_t len)
{
	for (;;) {
		ssize_t n = read(*(int *)ctx, buf, len);

		if (n > 0)
			return (int)n;
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			timed_out = 1;
		return -1;
	}
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len)
{
	for (;;) {
		ssize_t n = write(*(int *)ctx, buf, len);

		if (n > 0)
			return (int)n;
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			timed_out = 1;
		return -1;
	}
}

/* ------------------------------------------------------------------ */
/* Trust on first use: certificate fingerprints in KNOWN_HOSTS         */
/* Lines: "host:port <sha256 hex> <unix time first seen>"              */

enum { CERT_NEW, CERT_KNOWN, CERT_REPLACED, CERT_CHANGED };
static const char *const cert_status_names[] = { "new", "known", "replaced", "changed" };

static int known_lookup(const char *key, char *fp_out)
{
	FILE *f = fopen(KNOWN_HOSTS, "r");
	char line[512], k[300], fp[65];
	int found = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%299s %64s", k, fp) == 2 && !strcmp(k, key)) {
			strcpy(fp_out, fp);
			found = 1;
		}
	fclose(f);
	return found;
}

/* Add or replace the entry for key. Small file, rewritten whole and renamed. */
static void known_store(const char *key, const char *fp)
{
	char tmp[] = KNOWN_HOSTS ".XXXXXX", line[512], k[300];
	FILE *in, *out;
	int fd;

	mkdir("/srv", 0755);
	mkdir(KNOWN_DIR, 0755);
	if ((fd = mkstemp(tmp)) < 0 || !(out = fdopen(fd, "w"))) {
		syslog(LOG_WARNING, "can't write %s: %s", KNOWN_HOSTS, strerror(errno));
		return;
	}
	if ((in = fopen(KNOWN_HOSTS, "r"))) {
		while (fgets(line, sizeof(line), in))
			if (sscanf(line, "%299s", k) != 1 || strcmp(k, key))
				fputs(line, out);
		fclose(in);
	}
	fprintf(out, "%s %s %ld\n", key, fp, (long)time(NULL));
	if (fflush(out) || fsync(fd) || fclose(out) || rename(tmp, KNOWN_HOSTS)) {
		syslog(LOG_WARNING, "can't update %s: %s", KNOWN_HOSTS, strerror(errno));
		unlink(tmp);
	}
}

/* A BearSSL X.509 engine that accepts any chain, keeps the server's key
 * (from the first certificate) and checks that certificate's fingerprint. */
struct tofu {
	const br_x509_class *vtable;
	br_x509_decoder_context dec;
	br_sha256_context sha;
	int ncerts, in_first, decode_err;
	unsigned char hash[32];
	const char *key;          /* "host:port" */
	const char *trust;        /* trust=<hex> from the request, or NULL */
	int status;               /* CERT_* */
	char fp[65], known_fp[65];
};

static void dn_ignore(void *ctx, const void *buf, size_t len) { (void)ctx; (void)buf; (void)len; }

static void tofu_start_chain(const br_x509_class **ctx, const char *server_name)
{
	struct tofu *t = (struct tofu *)ctx;

	(void)server_name;
	t->ncerts = 0;
	t->decode_err = 0;
}

static void tofu_start_cert(const br_x509_class **ctx, uint32_t length)
{
	struct tofu *t = (struct tofu *)ctx;

	(void)length;
	t->in_first = t->ncerts++ == 0;
	if (t->in_first) {
		br_x509_decoder_init(&t->dec, dn_ignore, NULL);
		br_sha256_init(&t->sha);
	}
}

static void tofu_append(const br_x509_class **ctx, const unsigned char *buf, size_t len)
{
	struct tofu *t = (struct tofu *)ctx;

	if (t->in_first) {
		br_x509_decoder_push(&t->dec, buf, len);
		br_sha256_update(&t->sha, buf, len);
	}
}

static void tofu_end_cert(const br_x509_class **ctx)
{
	struct tofu *t = (struct tofu *)ctx;

	if (!t->in_first)
		return;
	t->in_first = 0;
	br_sha256_out(&t->sha, t->hash);
	if (br_x509_decoder_last_error(&t->dec))
		t->decode_err = br_x509_decoder_last_error(&t->dec);
}

static unsigned tofu_end_chain(const br_x509_class **ctx)
{
	struct tofu *t = (struct tofu *)ctx;

	if (!t->ncerts)
		return BR_ERR_X509_EMPTY_CHAIN;
	if (t->decode_err)
		return t->decode_err;
	if (!br_x509_decoder_get_pkey(&t->dec))
		return BR_ERR_X509_UNSUPPORTED;
	hex(t->hash, 32, t->fp);
	if (!known_lookup(t->key, t->known_fp)) {
		t->status = CERT_NEW;
		known_store(t->key, t->fp);
	} else if (!strcmp(t->known_fp, t->fp)) {
		t->status = CERT_KNOWN;
	} else if (t->trust && !strcasecmp(t->trust, t->fp)) {
		t->status = CERT_REPLACED;
		known_store(t->key, t->fp);
		syslog(LOG_NOTICE, "%s: certificate replaced on request (was %s, now %s)", t->key, t->known_fp, t->fp);
	} else {
		t->status = CERT_CHANGED;
		return BR_ERR_X509_NOT_TRUSTED;
	}
	return 0;
}

static const br_x509_pkey *tofu_get_pkey(const br_x509_class *const *ctx, unsigned *usages)
{
	struct tofu *t = (struct tofu *)ctx;

	if (usages)
		*usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
	return br_x509_decoder_get_pkey(&t->dec);
}

static const br_x509_class tofu_vtable = {
	sizeof(struct tofu), tofu_start_chain, tofu_start_cert, tofu_append,
	tofu_end_cert, tofu_end_chain, tofu_get_pkey
};

/* ------------------------------------------------------------------ */

static br_ssl_client_context sc;
static br_x509_minimal_context xc;   /* only to satisfy init_full; replaced below */
static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
static struct tofu tofu;

static void tls_fail(const struct gurl *u)
{
	int err = br_ssl_engine_last_error(&sc.eng);
	char hdr[300];

	if (tofu.status == CERT_CHANGED) {
		snprintf(hdr, sizeof(hdr), "X-Gemini-Cert: %s\r\nX-Gemini-Cert-Known: %s\r\nX-Gemini-Cert-Status: changed\r\n",
		         tofu.fp, tofu.known_fp);
		fail(502, "cert-changed", hdr,
		     "%s presented a different certificate than the one stored when it was first visited.\n"
		     "Stored:    %s\nPresented: %s\n"
		     "If the change is expected, repeat the request with trust=%s.",
		     u->host, tofu.known_fp, tofu.fp, tofu.fp);
	}
	if (timed_out)
		fail(504, "timeout", NULL, "%s stopped answering (no data for %d s).", u->host, IO_TIMEOUT);
	snprintf(hdr, sizeof(hdr), "X-Gateway-Detail: bearssl-%d\r\n", err);
	fail(502, "tls", hdr, "TLS with %s failed (BearSSL error %d).%s", u->host, err,
	     err == BR_ERR_BAD_VERSION ? " The server may require TLS 1.3, which the gateway doesn't support." : "");
}

int main(void)
{
	const char *method = getenv("REQUEST_METHOD"), *qs = getenv("QUERY_STRING");
	char *url, *trust, key[HOST_MAX + 8], buf[4096], header[HEADER_MAX + 1], *meta, *nl;
	struct gurl u;
	br_sslio_context ioc;
	size_t have = 0;
	int fd, n, status;

	openlog("gemgate", 0, LOG_DAEMON);
	allow_private = getenv("GEMGATE_ALLOW_PRIVATE") && !strcmp(getenv("GEMGATE_ALLOW_PRIVATE"), "1");
	if (method && !strcmp(method, "OPTIONS")) {
		fputs("Status: 204 No Content\r\nAccess-Control-Allow-Methods: GET\r\n", stdout);
		common_headers();
		fputs("\r\n", stdout);
		return 0;
	}
	if (method && strcmp(method, "GET") && strcmp(method, "HEAD"))
		fail(405, "bad-request", NULL, "Only GET is supported.");
	if (!qs || !(url = query_param(qs, "url")))
		fail(400, "bad-request", NULL, "Usage: ?url=<percent-encoded gemini:// URL>[&trust=<sha256 fingerprint>]");
	trust = query_param(qs, "trust");
	parse_url(url, &u);
	snprintf(key, sizeof(key), "%s:%s", u.host, u.port);

	fd = connect_host(&u);
	br_ssl_client_init_full(&sc, &xc, NULL, 0);
	br_ssl_engine_set_versions(&sc.eng, BR_TLS12, BR_TLS12);   /* Gemini requires 1.2+; BearSSL has no 1.3 */
	tofu.vtable = &tofu_vtable;
	tofu.key = key;
	tofu.trust = trust;
	br_ssl_engine_set_x509(&sc.eng, &tofu.vtable);
	br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof(iobuf), 1);
	if (!br_ssl_client_reset(&sc, u.host, 0))
		tls_fail(&u);
	br_sslio_init(&ioc, &sc.eng, sock_read, &fd, sock_write, &fd);

	if (br_sslio_write_all(&ioc, u.request, strlen(u.request)) < 0 ||
	    br_sslio_write_all(&ioc, "\r\n", 2) < 0 || br_sslio_flush(&ioc) < 0)
		tls_fail(&u);

	/* Response header: two-digit status, space, meta (up to 1024 bytes), CRLF. */
	for (;;) {
		if ((nl = memchr(buf, '\n', have)))
			break;
		if (have >= HEADER_MAX)
			fail(502, "bad-response", NULL, "%s sent a response header longer than Gemini allows.", u.host);
		n = br_sslio_read(&ioc, buf + have, sizeof(buf) - have);
		if (n < 0) {
			if (!have && br_ssl_engine_last_error(&sc.eng) != BR_ERR_OK && br_ssl_engine_last_error(&sc.eng) != BR_ERR_IO)
				tls_fail(&u);
			if (timed_out)
				tls_fail(&u);
			fail(502, "bad-response", NULL, "%s closed the connection without a complete response header.", u.host);
		}
		have += n;
	}
	memcpy(header, buf, nl - buf);
	header[nl - buf] = 0;
	if ((n = strlen(header)) && header[n - 1] == '\r')
		header[n - 1] = 0;
	if (!isdigit((unsigned char)header[0]) || !isdigit((unsigned char)header[1]) ||
	    (header[2] && header[2] != ' ' && header[2] != '\t'))
		fail(502, "bad-response", NULL, "%s sent an invalid response header.", u.host);
	status = (header[0] - '0') * 10 + header[1] - '0';
	meta = header[2] ? header + 3 : header + 2;

	fputs("Status: 200 OK\r\n", stdout);
	common_headers();
	printf("X-Gemini-Status: %d\r\nX-Gemini-Meta: ", status);
	put_encoded(meta);
	printf("\r\nX-Gemini-Cert: %s\r\nX-Gemini-Cert-Status: %s\r\n", tofu.fp, cert_status_names[tofu.status]);
	if (status / 10 == 2) {
		const char *ct = *meta ? meta : "text/gemini; charset=utf-8";
		const char *c;

		for (c = ct; *c; c++)
			if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7e)
				ct = "application/octet-stream";
		printf("Content-Type: %s\r\n\r\n", ct);
		if (method && !strcmp(method, "HEAD"))
			return 0;
		fwrite(nl + 1, 1, have - (nl + 1 - buf), stdout);
		while ((n = br_sslio_read(&ioc, buf, sizeof(buf))) > 0)
			if (fwrite(buf, 1, n, stdout) != (size_t)n)
				break;   /* client went away */
	} else {
		fputs("Content-Type: text/plain; charset=utf-8\r\n\r\n", stdout);
	}
	fflush(stdout);
	br_sslio_close(&ioc);
	close(fd);
	return 0;
}
