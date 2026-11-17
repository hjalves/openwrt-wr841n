/*
 * weatherd: receive a weather station's "customized" uploads (Ecowitt
 * protocol, an HTTP form POST about once a minute), keep the history and
 * expose it on ubus as "weather". No internet needed.
 *
 * Station side: Fine Offset / Ecowitt consoles (WS2900 and others) can post
 * to a custom server. weatherd listens on a TCP port (default 8080), accepts
 * only the configured station address and PASSKEY, converts the readings to
 * metric and stores them like huawei-trafficd does: every reading in RAM for
 * a few days, hourly summaries in RAM for a year and, optionally, the hourly
 * summaries on flash in compressed monthly files (13 months kept).
 *
 * Optionally, each accepted upload is also repeated, unchanged, to other
 * servers (plain HTTP POST; the router has no TLS), without ever blocking
 * the receiving side.
 *
 * Only the router's existing shared libraries are used: libubus, libubox,
 * libm and uClibc.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <netdb.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <libubus.h>
#include <libubox/blobmsg.h>
#include <libubox/usock.h>

#define MIN_VALID_TIME    1577836800u /* 2020-01-01: clock not set before this */
#define QUERY_DEFAULT_MAX 1000
#define QUERY_HARD_MAX    4000        /* keeps replies well under ubus' 1 MiB */
#define STATE_MAGIC       0x57544852u /* "WTHR" */
#define STATE_VERSION     1u
#define REQ_MAX           4096        /* an upload is about 600 bytes */
#define REQ_TIMEOUT_MS    5000
#define MAX_CONNS         4
#ifndef MIN_GAP
#define MIN_GAP           10          /* seconds: a second upload sooner is a retry, ignored */
#endif

/* "No reading" markers. */
#define T_NONE   INT16_MIN
#define H_NONE   255
#define U_NONE   0xffffu
#define UV_NONE  255
#define B_NONE   255

/*
 * One sample. A fine sample is one upload (n = 1, min = max = value); a
 * coarse sample summarises a period (default an hour) of fine samples.
 */
struct sample {
	uint32_t time;          /* Unix time on the router (period start for coarse) */
	uint32_t rain_total;    /* station's rain counter at the end, 0.01 mm */
	int16_t t_out, t_min, t_max;  /* outdoor temperature: average, lowest, highest, 0.1 °C */
	int16_t t_in;           /* indoor (console) temperature, 0.1 °C */
	uint16_t p_rel, p_abs;  /* pressure, relative (sea level) and absolute, 0.1 hPa */
	uint16_t wind, gust;    /* wind speed average and highest gust, 0.1 m/s */
	uint16_t dir;           /* wind direction, degrees (vector average) */
	uint16_t rain;          /* rain during the period, 0.01 mm */
	uint16_t rain_rate;     /* highest rain rate, 0.1 mm/h */
	uint16_t solar, solar_max;    /* solar radiation average and highest, W/m² */
	uint16_t n;             /* uploads behind this sample */
	uint8_t h_out, h_in;    /* relative humidity outdoor and indoor, % */
	uint8_t uv;             /* highest UV index */
	uint8_t batt;           /* outdoor sensor battery: 0 ok, 1 low */
};
typedef char sample_is_40_bytes[sizeof(struct sample) == 40 ? 1 : -1];

static void sample_clear(struct sample *s)
{
	memset(s, 0, sizeof(*s));
	s->t_out = s->t_min = s->t_max = s->t_in = T_NONE;
	s->p_rel = s->p_abs = s->wind = s->gust = s->dir = s->rain_rate = U_NONE;
	s->solar = s->solar_max = U_NONE;
	s->h_out = s->h_in = H_NONE;
	s->uv = UV_NONE;
	s->batt = B_NONE;
}

/* ---- configuration ---- */
static const char *cfg_port = "8080";
static const char *cfg_station;    /* accepted source address; NULL = any */
static struct in_addr station_addr;
static const char *cfg_passkey;    /* required PASSKEY; NULL = not checked */
static const char *cfg_socket;     /* ubus socket, NULL = default */
static unsigned cfg_save_every;    /* seconds, 0 = only on exit */
static const char *cfg_flash_dir;  /* hourly history on flash; NULL = off */
static unsigned cfg_flash_every = 86400;

struct ring {
	const char *name;
	const char *state;       /* optional persistence file */
	unsigned interval;       /* seconds; coarse only, 0 disables the coarse tier */
	unsigned cap, head, count;
	struct sample *buf;
};

static struct ring fine   = { .name = "fine",   .cap = 4320 };
static struct ring coarse = { .name = "coarse", .interval = 3600, .cap = 9600 };

/* ---- status ---- */
static uint32_t last_upload, accepted, rejected;
static char last_error[96];
static char station_type[32], station_model[32];
static bool have_rain_base;
static uint32_t rain_base;         /* last rain counter seen, 0.01 mm */
static uint32_t rain_base_time;    /* when it was seen */

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

/* ------------------------------------------------------------------ */
/* Hourly summaries                                                    */
/* ------------------------------------------------------------------ */

/* Replace coarse sample c (which starts as a copy of its period's first
 * fine sample) by the summary of the fine samples in [c->time, end). */
static void coarse_finalize(struct sample *c, uint32_t end)
{
	long t = 0, ti = 0, ho = 0, hi = 0, pr = 0, pa = 0, w = 0, so = 0;
	unsigned nt = 0, nti = 0, nho = 0, nhi = 0, npr = 0, npa = 0, nw = 0, nso = 0, nd = 0, n = 0, i;
	double dx = 0, dy = 0;
	uint32_t rain = 0;
	struct sample a;

	sample_clear(&a);
	for (i = fine.count; i-- > 0;) {
		const struct sample *f = ring_at(&fine, i);

		if (f->time < c->time)
			break;
		if (f->time >= end)
			continue;
		n++;
		if (f->t_out != T_NONE) {
			t += f->t_out; nt++;
			if (a.t_min == T_NONE || f->t_min < a.t_min) a.t_min = f->t_min;
			if (a.t_max == T_NONE || f->t_max > a.t_max) a.t_max = f->t_max;
		}
		if (f->t_in != T_NONE) { ti += f->t_in; nti++; }
		if (f->h_out != H_NONE) { ho += f->h_out; nho++; }
		if (f->h_in != H_NONE) { hi += f->h_in; nhi++; }
		if (f->p_rel != U_NONE) { pr += f->p_rel; npr++; }
		if (f->p_abs != U_NONE) { pa += f->p_abs; npa++; }
		if (f->wind != U_NONE) { w += f->wind; nw++; }
		if (f->gust != U_NONE && (a.gust == U_NONE || f->gust > a.gust)) a.gust = f->gust;
		if (f->dir != U_NONE) {  /* direction: average of unit vectors */
			dx += sin(f->dir * M_PI / 180);
			dy += cos(f->dir * M_PI / 180);
			nd++;
		}
		rain += f->rain;
		if (f->rain_rate != U_NONE && (a.rain_rate == U_NONE || f->rain_rate > a.rain_rate)) a.rain_rate = f->rain_rate;
		if (f->solar != U_NONE) { so += f->solar; nso++; }
		if (f->solar_max != U_NONE && (a.solar_max == U_NONE || f->solar_max > a.solar_max)) a.solar_max = f->solar_max;
		if (f->uv != UV_NONE && (a.uv == UV_NONE || f->uv > a.uv)) a.uv = f->uv;
		if (f->batt != B_NONE && (a.batt == B_NONE || f->batt > a.batt)) a.batt = f->batt;
		if (n == 1)
			a.rain_total = f->rain_total;  /* newest first: the period's last counter */
	}
	if (!n)
		return;
	a.time = c->time;
	a.n = n > 65535 ? 65535 : (uint16_t)n;
	if (nt) a.t_out = (int16_t)div_round(t, nt);
	if (nti) a.t_in = (int16_t)div_round(ti, nti);
	if (nho) a.h_out = (uint8_t)div_round(ho, nho);
	if (nhi) a.h_in = (uint8_t)div_round(hi, nhi);
	if (npr) a.p_rel = (uint16_t)div_round(pr, npr);
	if (npa) a.p_abs = (uint16_t)div_round(pa, npa);
	if (nw) a.wind = (uint16_t)div_round(w, nw);
	if (nso) a.solar = (uint16_t)div_round(so, nso);
	if (nd && (fabs(dx) > 1e-6 || fabs(dy) > 1e-6)) {
		int d = (int)lround(atan2(dx, dy) * 180 / M_PI);

		a.dir = (uint16_t)((d % 360 + 360) % 360);
	}
	a.rain = rain > 65535 ? 65535 : (uint16_t)rain;
	*c = a;
}

/* Keep s in the coarse tier if it starts a new period. */
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
	coarse.buf[(coarse.head + coarse.count - 1) % coarse.cap].time =
		s->time / coarse.interval * coarse.interval;  /* periods start on the hour */
}

static void record_sample(const struct sample *s)
{
	ring_push(&fine, s);
	coarse_offer(s);
}

/* ------------------------------------------------------------------ */
/* Persistence of the RAM tiers (optional)                             */
/* ------------------------------------------------------------------ */

static void ring_save(const struct ring *r)
{
	char tmp[PATH_MAX];
	uint32_t hdr[4] = { STATE_MAGIC, STATE_VERSION, r->count, r->interval };
	unsigned i;
	FILE *f;

	if (!r->state || snprintf(tmp, sizeof(tmp), "%s.tmp", r->state) >= (int)sizeof(tmp))
		return;
	if (!(f = fopen(tmp, "wb"))) {
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
	if (fread(hdr, sizeof(hdr), 1, f) != 1 || hdr[0] != STATE_MAGIC || hdr[1] != STATE_VERSION) {
		syslog(LOG_WARNING, "ignoring unrecognised state file %s", r->state);
		fclose(f);
		return;
	}
	for (i = 0; i < hdr[2] && fread(&s, sizeof(s), 1, f) == 1; i++)
		ring_push(r, &s);  /* keeps only the newest r->cap */
	syslog(LOG_INFO, "loaded %u %s samples from %s", r->count, r->name, r->state);
	fclose(f);
}

/* ------------------------------------------------------------------ */
/* Hourly history on flash (optional): compressed, append-only         */
/* ------------------------------------------------------------------ */
/*
 * Finished coarse samples are appended to one file per month (UTC),
 * <dir>/YYYY-MM.log, once every flash_every seconds and on clean stop.
 * Whole old months are deleted, so files are never rewritten.
 *
 * Each append is one block: 12-byte header + delta-encoded records.
 *   "WB", version 1, record count, payload length (u16 LE), 0 (u16),
 *   CRC-32 of the payload (u32 LE)
 * The first record of a block is encoded against an all-"none" sample, so
 * every block decodes on its own and a torn write loses only that block.
 * Record: a flags byte; varints for the time step minus the period, the
 * upload count, rain and the rain counter's change minus the rain; then per
 * flagged group its values, mostly as changes from the previous record:
 *   temperature: average change, average-min, max-average (0.1 °C)
 *   indoor: temperature change, humidity byte
 *   humidity: byte
 *   pressure: relative and absolute changes (0.1 hPa)
 *   wind: average, gust-average (0.1 m/s), direction change (-180..179)
 *   rain rate (0.1 mm/h)
 *   sun: average, peak-average (W/m²), UV byte
 *   flag 0x80: outdoor sensor battery low
 * About 20 bytes per hourly record.
 */
#define FLASH_MONTHS     13           /* keep this many monthly files */
#define FLASH_MIN_FREE   (64 * 1024)  /* never fill the overlay past this */
#define BLOCK_HDR        12
#define BLOCK_MAX        255          /* records per block */
#define REC_MAX          64           /* worst case about 52 bytes */
#define F_TOUT           0x01         /* t_out, t_min, t_max */
#define F_IN             0x02         /* t_in, h_in */
#define F_HOUT           0x04
#define F_PRESS          0x08         /* p_rel, p_abs */
#define F_WIND           0x10         /* wind, gust, dir */
#define F_RRATE          0x20
#define F_SUN            0x40         /* solar, solar_max, uv */
#define F_BATT_LOW       0x80
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

/* Deltas of values that may be "none": against the last known value, else 0. */
static int ref16(int16_t v) { return v == T_NONE ? 0 : v; }
static long refu(uint16_t v) { return v == U_NONE ? 0 : v; }

static int dir_delta(int a, int b)  /* b - a wrapped to -180..179 */
{
	int d = ((b - a) % 360 + 540) % 360 - 180;

	return d;
}

static size_t rec_encode(uint8_t *p, const struct sample *a, const struct sample *b)
{
	unsigned f = 0;
	size_t n = 0;

	if (b->t_out != T_NONE) f |= F_TOUT;
	if (b->t_in != T_NONE && b->h_in != H_NONE) f |= F_IN;
	if (b->h_out != H_NONE) f |= F_HOUT;
	if (b->p_rel != U_NONE && b->p_abs != U_NONE) f |= F_PRESS;
	if (b->wind != U_NONE && b->gust != U_NONE && b->dir != U_NONE) f |= F_WIND;
	if (b->rain_rate != U_NONE) f |= F_RRATE;
	if (b->solar != U_NONE && b->solar_max != U_NONE && b->uv != UV_NONE) f |= F_SUN;
	if (b->batt == 1) f |= F_BATT_LOW;
	p[n++] = (uint8_t)f;
	n += put_sv(p + n, (int64_t)b->time - a->time - (a->time ? (int64_t)coarse.interval : 0));
	n += put_uv(p + n, b->n);
	n += put_uv(p + n, b->rain);
	n += put_sv(p + n, (int64_t)b->rain_total - a->rain_total - b->rain);
	if (f & F_TOUT) {
		n += put_sv(p + n, b->t_out - ref16(a->t_out));
		n += put_uv(p + n, (uint64_t)(b->t_out - b->t_min));
		n += put_uv(p + n, (uint64_t)(b->t_max - b->t_out));
	}
	if (f & F_IN) {
		n += put_sv(p + n, b->t_in - ref16(a->t_in));
		p[n++] = b->h_in;
	}
	if (f & F_HOUT)
		p[n++] = b->h_out;
	if (f & F_PRESS) {
		n += put_sv(p + n, (long)b->p_rel - refu(a->p_rel));
		n += put_sv(p + n, (long)b->p_abs - refu(a->p_abs));
	}
	if (f & F_WIND) {
		n += put_uv(p + n, b->wind);
		n += put_uv(p + n, (uint64_t)(b->gust >= b->wind ? b->gust - b->wind : 0));
		n += put_sv(p + n, dir_delta(a->dir == U_NONE ? 0 : a->dir, b->dir));
	}
	if (f & F_RRATE)
		n += put_uv(p + n, b->rain_rate);
	if (f & F_SUN) {
		n += put_uv(p + n, b->solar);
		n += put_uv(p + n, (uint64_t)(b->solar_max >= b->solar ? b->solar_max - b->solar : 0));
		p[n++] = b->uv;
	}
	return n;
}

static bool rec_decode(const uint8_t **p, const uint8_t *end, const struct sample *a, struct sample *b)
{
	uint64_t u, v;
	int64_t s, s2;
	unsigned f;

	sample_clear(b);
	if (*p >= end)
		return false;
	f = *(*p)++;
	if (!get_sv(p, end, &s))
		return false;
	b->time = (uint32_t)((int64_t)a->time + s + (a->time ? (int64_t)coarse.interval : 0));
	if (!get_uv(p, end, &u) || u > 65535)
		return false;
	b->n = (uint16_t)u;
	if (!get_uv(p, end, &u) || u > 65535 || !get_sv(p, end, &s))
		return false;
	b->rain = (uint16_t)u;
	b->rain_total = (uint32_t)((int64_t)a->rain_total + s + (int64_t)u);
	if (f & F_TOUT) {
		if (!get_sv(p, end, &s) || !get_uv(p, end, &u) || !get_uv(p, end, &v))
			return false;
		b->t_out = (int16_t)(ref16(a->t_out) + s);
		b->t_min = (int16_t)(b->t_out - (int64_t)u);
		b->t_max = (int16_t)(b->t_out + (int64_t)v);
	}
	if (f & F_IN) {
		if (!get_sv(p, end, &s) || *p >= end)
			return false;
		b->t_in = (int16_t)(ref16(a->t_in) + s);
		b->h_in = *(*p)++;
	}
	if (f & F_HOUT) {
		if (*p >= end)
			return false;
		b->h_out = *(*p)++;
	}
	if (f & F_PRESS) {
		if (!get_sv(p, end, &s) || !get_sv(p, end, &s2))
			return false;
		b->p_rel = (uint16_t)(refu(a->p_rel) + s);
		b->p_abs = (uint16_t)(refu(a->p_abs) + s2);
	}
	if (f & F_WIND) {
		if (!get_uv(p, end, &u) || !get_uv(p, end, &v) || u + v > 65534 || !get_sv(p, end, &s) ||
		    s < -180 || s > 179)
			return false;
		b->wind = (uint16_t)u;
		b->gust = (uint16_t)(u + v);
		b->dir = (uint16_t)(((a->dir == U_NONE ? 0 : a->dir) + s + 360) % 360);
	}
	if (f & F_RRATE) {
		if (!get_uv(p, end, &u) || u > 65534)
			return false;
		b->rain_rate = (uint16_t)u;
	}
	if (f & F_SUN) {
		if (!get_uv(p, end, &u) || !get_uv(p, end, &v) || u + v > 65534 || *p >= end)
			return false;
		b->solar = (uint16_t)u;
		b->solar_max = (uint16_t)(u + v);
		b->uv = *(*p)++;
	}
	b->batt = f & F_BATT_LOW ? 1 : (b->n ? 0 : B_NONE);
	return true;
}

static void zero_sample(struct sample *z)
{
	sample_clear(z);
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
		if (hdr[0] != 'W' || hdr[1] != 'B' || hdr[2] != 1 || !count)
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

	hdr[0] = 'W'; hdr[1] = 'B'; hdr[2] = 1; hdr[3] = (uint8_t)count;
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
	if (coarse.interval) {
		ring_load(&coarse);
		if (cfg_flash_dir)
			flash_load();
		if (!coarse.count)
			for (i = 0; i < fine.count; i++)
				coarse_offer(ring_at(&fine, i));
	}
	/* The rain counter continues from the newest known sample. */
	if (fine.count) {
		rain_base = ring_at(&fine, fine.count - 1)->rain_total;
		rain_base_time = ring_at(&fine, fine.count - 1)->time;
		have_rain_base = true;
	} else if (coarse.count) {
		rain_base = ring_at(&coarse, coarse.count - 1)->rain_total;
		rain_base_time = ring_at(&coarse, coarse.count - 1)->time + coarse.interval;
		have_rain_base = true;
	}
}

static struct uloop_timeout save_timer;

static void save_timer_cb(struct uloop_timeout *t)
{
	state_save();
	uloop_timeout_set(t, cfg_save_every * 1000);
}

static unsigned nfwds;                     /* forwarding targets (below) */
static void fwd_submit(const char *body);

/* ------------------------------------------------------------------ */
/* The station's uploads: a small HTTP server                          */
/* ------------------------------------------------------------------ */

/* Decode application/x-www-form-urlencoded in place. */
static void url_decode(char *s)
{
	char *o = s;

	for (; *s; s++) {
		if (*s == '+') {
			*o++ = ' ';
		} else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
			char h[3] = { s[1], s[2], 0 };

			*o++ = (char)strtol(h, NULL, 16);
			s += 2;
		} else {
			*o++ = *s;
		}
	}
	*o = '\0';
}

/* A number field; false if absent or not a number. */
static bool num(char **keys, char **vals, unsigned n, const char *key, double *out)
{
	unsigned i;

	for (i = 0; i < n; i++)
		if (!strcmp(keys[i], key)) {
			char *end;
			double v = strtod(vals[i], &end);

			if (end == vals[i] || *end || v != v || v > 1e9 || v < -1e9)
				return false;
			*out = v;
			return true;
		}
	return false;
}

static const char *str(char **keys, char **vals, unsigned n, const char *key)
{
	unsigned i;

	for (i = 0; i < n; i++)
		if (!strcmp(keys[i], key))
			return vals[i];
	return NULL;
}

static uint16_t u16(double v, double max)  /* clamp to 0..max, round */
{
	return v < 0 ? 0 : v > max ? (uint16_t)max : (uint16_t)lround(v);
}

/* Parse an Ecowitt upload body into a sample. Returns an error text or NULL. */
static const char *parse_upload(char *body, struct sample *s, uint32_t now)
{
	char *keys[48], *vals[48], *tok, *save = NULL;
	unsigned n = 0;
	double v;
	const char *pk;

	for (tok = strtok_r(body, "&", &save); tok && n < 48; tok = strtok_r(NULL, "&", &save)) {
		char *eq = strchr(tok, '=');

		if (!eq)
			continue;
		*eq = '\0';
		url_decode(tok);
		url_decode(eq + 1);
		keys[n] = tok;
		vals[n++] = eq + 1;
	}
	pk = str(keys, vals, n, "PASSKEY");
	if (cfg_passkey && (!pk || strcmp(pk, cfg_passkey)))
		return "wrong or missing PASSKEY";
	if (!pk && !str(keys, vals, n, "stationtype"))
		return "not a weather station upload";

	sample_clear(s);
	s->time = now;
	s->n = 1;
	if (num(keys, vals, n, "tempf", &v) && v > -100 && v < 200)
		s->t_out = s->t_min = s->t_max = (int16_t)lround((v - 32) * 50 / 9);
	if (num(keys, vals, n, "tempinf", &v) && v > -100 && v < 200)
		s->t_in = (int16_t)lround((v - 32) * 50 / 9);
	if (num(keys, vals, n, "humidity", &v) && v >= 0 && v <= 100)
		s->h_out = (uint8_t)lround(v);
	if (num(keys, vals, n, "humidityin", &v) && v >= 0 && v <= 100)
		s->h_in = (uint8_t)lround(v);
	if (num(keys, vals, n, "baromrelin", &v) && v > 15 && v < 35)
		s->p_rel = u16(v * 338.639, 65534);
	if (num(keys, vals, n, "baromabsin", &v) && v > 15 && v < 35)
		s->p_abs = u16(v * 338.639, 65534);
	if (num(keys, vals, n, "windspeedmph", &v) && v >= 0 && v < 300)
		s->wind = u16(v * 4.4704, 65534);
	if (num(keys, vals, n, "windgustmph", &v) && v >= 0 && v < 300)
		s->gust = u16(v * 4.4704, 65534);
	if (num(keys, vals, n, "winddir", &v) && v >= 0 && v <= 360)
		s->dir = (uint16_t)(lround(v) % 360);
	if (num(keys, vals, n, "rainratein", &v) && v >= 0 && v < 100)
		s->rain_rate = u16(v * 254, 65534);
	if (num(keys, vals, n, "solarradiation", &v) && v >= 0 && v < 2000)
		s->solar = s->solar_max = u16(v, 65534);
	if (num(keys, vals, n, "uv", &v) && v >= 0 && v < 30)
		s->uv = (uint8_t)lround(v);
	if (num(keys, vals, n, "wh65batt", &v) && (v == 0 || v == 1))
		s->batt = (uint8_t)v;
	/*
	 * Rain: the station's own hourly/daily counters reset on its schedule,
	 * so use the never-resetting total and store the increase. A counter
	 * that goes down (station reset) or rises implausibly fast (more than
	 * 30 mm plus 2 mm per minute since the previous upload, at most 500 mm)
	 * starts a new baseline without counting any rain.
	 */
	if (num(keys, vals, n, "totalrainin", &v) && v >= 0 && v < 40000) {
		uint32_t total = (uint32_t)lround(v * 2540);  /* 0.01 mm */
		uint32_t mins = rain_base_time && now > rain_base_time ? (now - rain_base_time) / 60 : 0;
		uint32_t limit = 3000 + (mins > 23500 ? 47000 : mins * 200);

		if (limit > 50000)
			limit = 50000;
		if (have_rain_base && total >= rain_base && total - rain_base <= limit)
			s->rain = (uint16_t)(total - rain_base);
		else if (have_rain_base)
			syslog(LOG_NOTICE, "rain counter went from %u to %u (0.01 mm): new baseline", rain_base, total);
		s->rain_total = total;
	} else {
		s->rain_total = rain_base;
	}
	{
		const char *t = str(keys, vals, n, "stationtype"), *m = str(keys, vals, n, "model");

		if (t)
			snprintf(station_type, sizeof(station_type), "%s", t);
		if (m)
			snprintf(station_model, sizeof(station_model), "%s", m);
	}
	return NULL;
}

struct conn {
	struct uloop_fd fd;
	struct uloop_timeout timeout;
	struct in_addr peer;
	size_t len;
	char buf[REQ_MAX + 1];
};

static struct uloop_fd listen_fd = { .fd = -1 };
static unsigned nconns;

static void conn_close(struct conn *c)
{
	uloop_timeout_cancel(&c->timeout);
	uloop_fd_delete(&c->fd);
	close(c->fd.fd);
	free(c);
	nconns--;
}

static void conn_reply(struct conn *c, const char *status)
{
	char r[128];
	int n = snprintf(r, sizeof(r), "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);

	(void)send(c->fd.fd, r, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT);  /* best effort */
	conn_close(c);
}

static void reject(struct conn *c, const char *status, const char *why)
{
	rejected++;
	snprintf(last_error, sizeof(last_error), "%s from %s", why, inet_ntoa(c->peer));
	conn_reply(c, status);
}

/* A complete request is in c->buf: handle it. */
static void conn_request(struct conn *c, char *body)
{
	uint32_t now = (uint32_t)time(NULL);
	struct sample s;
	const char *err;
	char *raw = NULL;

	if (strncmp(c->buf, "POST ", 5)) {
		reject(c, "405 Method Not Allowed", "not a POST");
		return;
	}
	if (cfg_station && c->peer.s_addr != station_addr.s_addr) {
		reject(c, "403 Forbidden", "upload from an address other than the station's");
		return;
	}
	if (nfwds)
		raw = strdup(body);  /* parsing decodes the body in place */
	if ((err = parse_upload(body, &s, now))) {
		free(raw);
		reject(c, err[0] == 'w' ? "403 Forbidden" : "400 Bad Request", err);
		return;
	}
	if (fine.count && now >= MIN_VALID_TIME && now - ring_at(&fine, fine.count - 1)->time < MIN_GAP) {
		free(raw);
		conn_reply(c, "200 OK");  /* a retry of the previous upload */
		return;
	}
	if (raw) {
		fwd_submit(raw);  /* the receivers can rely on the station's own dateutc */
		free(raw);
	}
	if (now < MIN_VALID_TIME) {
		snprintf(last_error, sizeof(last_error), "system clock not set (waiting for NTP)");
		conn_reply(c, "200 OK");
		return;
	}
	rain_base = s.rain_total;
	rain_base_time = now;
	have_rain_base = true;
	record_sample(&s);
	accepted++;
	last_upload = now;
	last_error[0] = '\0';
	conn_reply(c, "200 OK");
}

static void conn_cb(struct uloop_fd *fd, unsigned int events)
{
	struct conn *c = container_of(fd, struct conn, fd);
	ssize_t r;
	char *hdr_end;
	bool eof = false;

	(void)events;
	for (;;) {
		if (c->len >= REQ_MAX) {
			reject(c, "413 Payload Too Large", "request too large");
			return;
		}
		r = recv(fd->fd, c->buf + c->len, REQ_MAX - c->len, MSG_DONTWAIT);
		if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
			eof = true;  /* the sender is done (or gone): use what arrived */
			break;
		}
		if (r < 0)
			break;
		c->len += (size_t)r;
	}
	c->buf[c->len] = '\0';
	if (!(hdr_end = strstr(c->buf, "\r\n\r\n"))) {
		if (eof)
			conn_close(c);
		return;  /* headers not complete yet */
	}
	{
		const char *cl = strcasestr(c->buf, "\r\nContent-Length:");
		size_t want = cl && cl < hdr_end ? strtoul(cl + 17, NULL, 10) : 0;
		size_t have = c->len - (size_t)(hdr_end + 4 - c->buf);

		if (want > REQ_MAX) {
			reject(c, "413 Payload Too Large", "request too large");
			return;
		}
		if (have < want) {
			if (eof)
				conn_close(c);  /* closed before the whole body arrived */
			return;
		}
		hdr_end[4 + want] = '\0';
	}
	conn_request(c, hdr_end + 4);
}

static void conn_timeout_cb(struct uloop_timeout *t)
{
	conn_close(container_of(t, struct conn, timeout));
}

static void listen_cb(struct uloop_fd *fd, unsigned int events)
{
	(void)events;
	for (;;) {
		struct sockaddr_in a;
		socklen_t al = sizeof(a);
		int s = accept(fd->fd, (struct sockaddr *)&a, &al);
		struct conn *c;

		if (s < 0)
			return;
		if (nconns >= MAX_CONNS || a.sin_family != AF_INET || !(c = calloc(1, sizeof(*c)))) {
			close(s);
			continue;
		}
		fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
		fcntl(s, F_SETFD, FD_CLOEXEC);
		nconns++;
		c->peer = a.sin_addr;
		c->fd.fd = s;
		c->fd.cb = conn_cb;
		c->timeout.cb = conn_timeout_cb;
		uloop_fd_add(&c->fd, ULOOP_READ);
		uloop_timeout_set(&c->timeout, REQ_TIMEOUT_MS);
	}
}

/* ------------------------------------------------------------------ */
/* Repeating uploads to other servers (optional)                       */
/* ------------------------------------------------------------------ */
/*
 * Each target gets the station's form body exactly as received, by plain
 * HTTP POST. Per target there is at most one request in flight and one
 * upload waiting (the newest; an older waiting one is dropped). DNS lookups
 * run in a short-lived child process, so a slow or dead resolver (LTE down)
 * never stalls the receiving side or ubus. Addresses are looked up again
 * every FWD_DNS_TTL seconds, or after a failed connection.
 */
#define FWD_MAX          4
#define FWD_TIMEOUT_MS   15000
#define FWD_DNS_TTL      600

struct fwd {
	const char *url;
	char host[128], port[8], path[256];
	struct in_addr addr;
	uint32_t addr_time;
	bool have_addr;
	struct uloop_process resolver;
	struct uloop_fd rfd;            /* pipe from the resolver child */
	bool resolving;
	struct uloop_fd fd;             /* the HTTP request */
	struct uloop_timeout timeout;
	bool busy, writing;
	char *req;
	size_t req_len, sent;
	char resp[64];
	size_t resp_len;
	char *pending;                  /* body waiting to be sent */
	uint32_t ok, failed, dropped, last_time;
	int last_status;
	char last_error[80];
};

static struct fwd fwds[FWD_MAX];

static bool fwd_parse_url(struct fwd *f, const char *url)
{
	const char *h, *slash, *colon;
	size_t hl;

	if (strncmp(url, "http://", 7))
		return false;  /* https needs TLS, which this router doesn't have */
	h = url + 7;
	slash = strchr(h, '/');
	hl = slash ? (size_t)(slash - h) : strlen(h);
	colon = memchr(h, ':', hl);
	if (!hl || (colon ? (size_t)(colon - h) : hl) >= sizeof(f->host))
		return false;
	snprintf(f->host, sizeof(f->host), "%.*s", (int)(colon ? colon - h : (long)hl), h);
	snprintf(f->port, sizeof(f->port), "%.*s", colon ? (int)(hl - (size_t)(colon - h) - 1) : 2, colon ? colon + 1 : "80");
	if (!f->port[0] || strspn(f->port, "0123456789") != strlen(f->port) || atoi(f->port) < 1 || atoi(f->port) > 65535)
		return false;
	snprintf(f->path, sizeof(f->path), "%s", slash ? slash : "/");
	f->url = url;
	return true;
}

static void fwd_kick(struct fwd *f);

static void fwd_fail(struct fwd *f, const char *what, bool forget_addr)
{
	f->failed++;
	f->last_status = 0;
	snprintf(f->last_error, sizeof(f->last_error), "%s", what);
	if (forget_addr)
		f->have_addr = false;  /* the address may have changed: look it up again */
}

static void fwd_close(struct fwd *f)
{
	uloop_timeout_cancel(&f->timeout);
	if (f->fd.fd >= 0) {
		uloop_fd_delete(&f->fd);
		close(f->fd.fd);
		f->fd.fd = -1;
	}
	free(f->req);
	f->req = NULL;
	f->busy = false;
}

static void fwd_done(struct fwd *f)
{
	fwd_close(f);
	fwd_kick(f);  /* a newer upload may be waiting */
}

static void fwd_timeout_cb(struct uloop_timeout *t)
{
	struct fwd *f = container_of(t, struct fwd, timeout);

	fwd_fail(f, "no answer within 15 s", true);
	fwd_done(f);
}

static void fwd_fd_cb(struct uloop_fd *u, unsigned int events)
{
	struct fwd *f = container_of(u, struct fwd, fd);
	ssize_t r;

	(void)events;
	if (f->writing) {
		int err = 0;
		socklen_t el = sizeof(err);

		if (!f->sent && (getsockopt(u->fd, SOL_SOCKET, SO_ERROR, &err, &el) || err)) {
			fwd_fail(f, err ? strerror(err) : "connect failed", true);
			fwd_done(f);
			return;
		}
		r = send(u->fd, f->req + f->sent, f->req_len - f->sent, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
			fwd_fail(f, strerror(errno), true);
			fwd_done(f);
			return;
		}
		if (r > 0)
			f->sent += (size_t)r;
		if (f->sent == f->req_len) {
			f->writing = false;
			uloop_fd_delete(u);
			uloop_fd_add(u, ULOOP_READ);
		}
		return;
	}
	/* Only the status line matters: "HTTP/1.x NNN ..." */
	while ((r = recv(u->fd, f->resp + f->resp_len, sizeof(f->resp) - 1 - f->resp_len, MSG_DONTWAIT)) > 0) {
		f->resp_len += (size_t)r;
		f->resp[f->resp_len] = '\0';
		if (strstr(f->resp, "\r\n") || f->resp_len == sizeof(f->resp) - 1)
			break;
	}
	if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !strstr(f->resp, "\r\n") && f->resp_len < sizeof(f->resp) - 1)
		return;  /* more to come */
	{
		int st = 0;

		if (sscanf(f->resp, "HTTP/%*d.%*d %d", &st) != 1) {
			fwd_fail(f, f->resp_len ? "unrecognised reply" : "connection closed without a reply", false);
		} else if (st >= 200 && st < 300) {
			f->ok++;
			f->last_status = st;
			f->last_time = (uint32_t)time(NULL);
			f->last_error[0] = '\0';
		} else {
			char e[80];

			snprintf(e, sizeof(e), "server answered %d", st);
			fwd_fail(f, e, false);
			f->last_status = st;
		}
	}
	fwd_done(f);
}

static void fwd_send(struct fwd *f)
{
	struct sockaddr_in a = { .sin_family = AF_INET };
	char *body = f->pending;
	size_t bl = strlen(body);
	int s, n;

	f->pending = NULL;
	f->req_len = 0;
	f->req = malloc(512 + bl);
	if (!f->req) {
		free(body);
		return;
	}
	n = snprintf(f->req, 512, "POST %s HTTP/1.1\r\nHost: %s%s%s\r\nUser-Agent: weatherd\r\n"
	             "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n"
	             "Connection: close\r\n\r\n", f->path, f->host, strcmp(f->port, "80") ? ":" : "",
	             strcmp(f->port, "80") ? f->port : "", bl);
	memcpy(f->req + n, body, bl);
	f->req_len = (size_t)n + bl;
	free(body);
	f->sent = f->resp_len = 0;
	f->resp[0] = '\0';
	a.sin_addr = f->addr;
	a.sin_port = htons((uint16_t)atoi(f->port));
	s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (s < 0 || (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0 && errno != EINPROGRESS)) {
		fwd_fail(f, strerror(errno), true);
		if (s >= 0)
			close(s);
		free(f->req);
		f->req = NULL;
		return;
	}
	f->busy = f->writing = true;
	f->fd.fd = s;
	f->fd.cb = fwd_fd_cb;
	uloop_fd_add(&f->fd, ULOOP_WRITE);
	f->timeout.cb = fwd_timeout_cb;
	uloop_timeout_set(&f->timeout, FWD_TIMEOUT_MS);
}

/* The resolver child's answer: 4 bytes of IPv4 address, or nothing. */
static void fwd_rfd_cb(struct uloop_fd *u, unsigned int events)
{
	struct fwd *f = container_of(u, struct fwd, rfd);
	struct in_addr a;
	ssize_t r = read(u->fd, &a, sizeof(a));

	(void)events;
	if (r < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	uloop_fd_delete(u);
	close(u->fd);
	u->fd = -1;
	f->resolving = false;
	if (r == (ssize_t)sizeof(a)) {
		f->addr = a;
		f->have_addr = true;
		f->addr_time = (uint32_t)time(NULL);
		fwd_kick(f);
	} else {
		snprintf(f->last_error, sizeof(f->last_error), "cannot look up %.60s", f->host);
		if (f->pending) {  /* nothing can be sent until the next upload retries the lookup */
			free(f->pending);
			f->pending = NULL;
			f->dropped++;
		}
	}
}

static void fwd_reaped(struct uloop_process *p, int status)
{
	(void)p; (void)status;
}

static void fwd_resolve(struct fwd *f)
{
	int pfd[2];
	pid_t pid;

	if (pipe(pfd) < 0)
		return;
	pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return;
	}
	if (!pid) {  /* child: one blocking lookup, then exit */
		struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;

		close(pfd[0]);
		alarm(30);
		if (!getaddrinfo(f->host, NULL, &hints, &res) && res) {
			struct in_addr a = ((struct sockaddr_in *)res->ai_addr)->sin_addr;

			if (write(pfd[1], &a, sizeof(a)) != (ssize_t)sizeof(a))
				_exit(1);
		}
		_exit(0);
	}
	close(pfd[1]);
	f->resolving = true;
	f->resolver.pid = pid;
	f->resolver.cb = fwd_reaped;
	uloop_process_add(&f->resolver);
	f->rfd.fd = pfd[0];
	f->rfd.cb = fwd_rfd_cb;
	fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
	uloop_fd_add(&f->rfd, ULOOP_READ);
}

static void fwd_kick(struct fwd *f)
{
	if (!f->pending || f->busy || f->resolving)
		return;
	if (!f->have_addr || (uint32_t)time(NULL) - f->addr_time > FWD_DNS_TTL) {
		fwd_resolve(f);
		return;
	}
	fwd_send(f);
}

/* Queue an accepted upload's original body for every target. */
static void fwd_submit(const char *body)
{
	unsigned i;

	for (i = 0; i < nfwds; i++) {
		struct fwd *f = &fwds[i];
		char *copy = strdup(body);

		if (!copy)
			continue;
		if (f->pending) {
			free(f->pending);
			f->dropped++;
		}
		f->pending = copy;
		fwd_kick(f);
	}
}

/* ------------------------------------------------------------------ */
/* ubus interface: weather                                             */
/* ------------------------------------------------------------------ */

static struct blob_buf b;

static void add_i(struct blob_buf *buf, const char *k, int v) { blobmsg_add_u32(buf, k, (uint32_t)v); }

static void add_sample(struct blob_buf *buf, const char *name, const struct sample *s)
{
	void *t = blobmsg_open_table(buf, name);

	add_i(buf, "time", (int)s->time);
	add_i(buf, "n", s->n);
	if (s->t_out != T_NONE) {
		add_i(buf, "temp", s->t_out);
		add_i(buf, "temp_min", s->t_min);
		add_i(buf, "temp_max", s->t_max);
	}
	if (s->h_out != H_NONE) add_i(buf, "hum", s->h_out);
	if (s->t_in != T_NONE) add_i(buf, "temp_in", s->t_in);
	if (s->h_in != H_NONE) add_i(buf, "hum_in", s->h_in);
	if (s->p_rel != U_NONE) add_i(buf, "press", s->p_rel);
	if (s->p_abs != U_NONE) add_i(buf, "press_abs", s->p_abs);
	if (s->wind != U_NONE) add_i(buf, "wind", s->wind);
	if (s->gust != U_NONE) add_i(buf, "gust", s->gust);
	if (s->dir != U_NONE) add_i(buf, "dir", s->dir);
	add_i(buf, "rain", s->rain);
	add_i(buf, "rain_total", (int)s->rain_total);
	if (s->rain_rate != U_NONE) add_i(buf, "rain_rate", s->rain_rate);
	if (s->solar != U_NONE) {
		add_i(buf, "solar", s->solar);
		add_i(buf, "solar_max", s->solar_max);
	}
	if (s->uv != UV_NONE) add_i(buf, "uv", s->uv);
	if (s->batt != B_NONE) blobmsg_add_u8(buf, "batt_low", s->batt);
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
	add_i(&b, "count", (int)q.n);
	blobmsg_add_u8(&b, "truncated", q.truncated);
	return ubus_send_reply(ctx, req, b.head);
}

static void add_ring_status(const struct ring *r)
{
	void *t = blobmsg_open_table(&b, r->name);

	add_i(&b, "interval", (int)r->interval);
	add_i(&b, "capacity", (int)r->cap);
	add_i(&b, "count", (int)r->count);
	if (r->count) {
		add_i(&b, "oldest", (int)ring_at(r, 0)->time);
		add_i(&b, "newest", (int)ring_at(r, r->count - 1)->time);
	}
	if (r->state)
		blobmsg_add_string(&b, "state_file", r->state);
	blobmsg_close_table(&b, t);
}

static int m_status(struct ubus_context *ctx, struct ubus_object *obj,
                    struct ubus_request_data *req, const char *method,
                    struct blob_attr *msg)
{
	void *t;

	(void)obj; (void)method; (void)msg;
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "port", cfg_port);
	if (cfg_station)
		blobmsg_add_string(&b, "station", cfg_station);
	blobmsg_add_u8(&b, "passkey_checked", cfg_passkey != NULL);
	if (station_type[0])
		blobmsg_add_string(&b, "station_type", station_type);
	if (station_model[0])
		blobmsg_add_string(&b, "station_model", station_model);
	add_ring_status(&fine);
	if (coarse.interval)
		add_ring_status(&coarse);
	add_i(&b, "now", (int)time(NULL));
	add_i(&b, "last_upload", (int)last_upload);
	add_i(&b, "accepted", (int)accepted);
	add_i(&b, "rejected", (int)rejected);
	if (last_error[0])
		blobmsg_add_string(&b, "last_error", last_error);
	t = blobmsg_open_table(&b, "units");  /* integers, scaled */
	blobmsg_add_string(&b, "temp", "0.1 °C");
	blobmsg_add_string(&b, "hum", "%");
	blobmsg_add_string(&b, "press", "0.1 hPa");
	blobmsg_add_string(&b, "wind", "0.1 m/s");
	blobmsg_add_string(&b, "dir", "degrees");
	blobmsg_add_string(&b, "rain", "0.01 mm");
	blobmsg_add_string(&b, "rain_rate", "0.1 mm/h");
	blobmsg_add_string(&b, "solar", "W/m²");
	blobmsg_add_string(&b, "uv", "index");
	blobmsg_close_table(&b, t);
	if (nfwds) {
		void *a = blobmsg_open_array(&b, "forward");
		unsigned i;

		for (i = 0; i < nfwds; i++) {
			const struct fwd *f = &fwds[i];

			t = blobmsg_open_table(&b, NULL);
			blobmsg_add_string(&b, "url", f->url);
			add_i(&b, "ok", (int)f->ok);
			add_i(&b, "failed", (int)f->failed);
			add_i(&b, "dropped", (int)f->dropped);
			if (f->last_status)
				add_i(&b, "last_status", f->last_status);
			if (f->last_time)
				add_i(&b, "last_ok", (int)f->last_time);
			if (f->last_error[0])
				blobmsg_add_string(&b, "last_error", f->last_error);
			if (f->have_addr)
				blobmsg_add_string(&b, "address", inet_ntoa(f->addr));
			blobmsg_close_table(&b, t);
		}
		blobmsg_close_array(&b, a);
	}
	if (cfg_flash_dir || flash_error[0]) {
		unsigned months[64], n = cfg_flash_dir ? flash_months(months, 64) : 0, i;
		uint64_t bytes = 0;
		char path[PATH_MAX];
		struct stat st;

		t = blobmsg_open_table(&b, "flash");
		for (i = 0; i < n; i++)
			if (month_file(path, sizeof(path), months[i]) && stat(path, &st) == 0)
				bytes += (uint64_t)st.st_size;
		blobmsg_add_u8(&b, "enabled", cfg_flash_dir != NULL);
		if (cfg_flash_dir) {
			blobmsg_add_string(&b, "dir", cfg_flash_dir);
			add_i(&b, "every", (int)cfg_flash_every);
			add_i(&b, "files", (int)n);
			blobmsg_add_u64(&b, "bytes", bytes);
			add_i(&b, "saved_until", (int)flushed_until);
			add_i(&b, "last_write", (int)flash_last_write);
		}
		if (flash_error[0])
			blobmsg_add_string(&b, "error", flash_error);
		blobmsg_close_table(&b, t);
	}
	return ubus_send_reply(ctx, req, b.head);
}

static const struct ubus_method methods[] = {
	UBUS_METHOD_NOARG("latest", m_latest),
	UBUS_METHOD("query", m_query, query_policy),
	UBUS_METHOD_NOARG("status", m_status),
};

static struct ubus_object_type obj_type = UBUS_OBJECT_TYPE("weather", methods);

static struct ubus_object ubus_obj = {
	.name = "weather",
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
		"  -p port      TCP port for the station's uploads (default 8080)\n"
		"  -a address   accept uploads only from this IPv4 address\n"
		"  -k passkey   accept uploads only with this PASSKEY\n"
		"  -n samples   fine history size: every upload (default 4320, 40 bytes each)\n"
		"  -c seconds   coarse tier period (default 3600, 0 disables it)\n"
		"  -N samples   coarse history size (default 9600, 40 bytes each)\n"
		"  -f file      fine history file: loaded at start, saved on exit\n"
		"  -F file      coarse history file (default: <fine file>.coarse)\n"
		"  -S seconds   also save the history files this often (default: exit only)\n"
		"  -D dir       keep the hourly history on flash in dir (compressed, monthly files)\n"
		"  -E seconds   append to the flash history this often (default 86400)\n"
		"  -u URL       also repeat each accepted upload to this http:// URL (repeatable, up to 4)\n"
		"  -s socket    ubus socket path\n", prog);
	exit(2);
}

int main(int argc, char **argv)
{
	static char coarse_default[PATH_MAX];
	int opt;

	while ((opt = getopt(argc, argv, "p:a:k:n:c:N:f:F:S:D:E:u:s:h")) != -1) {
		switch (opt) {
		case 'p': cfg_port = optarg; parse_uint(optarg, "port", 1, 65535); break;
		case 'a': cfg_station = *optarg ? optarg : NULL; break;
		case 'k': cfg_passkey = *optarg ? optarg : NULL; break;
		case 'n': fine.cap = parse_uint(optarg, "history size", 1, 1000000); break;
		case 'c': coarse.interval = parse_uint(optarg, "coarse period", 0, 604800); break;
		case 'N': coarse.cap = parse_uint(optarg, "coarse history size", 1, 1000000); break;
		case 'f': fine.state = optarg; break;
		case 'F': coarse.state = optarg; break;
		case 'S': cfg_save_every = parse_uint(optarg, "save interval", 60, 604800); break;
		case 'D': cfg_flash_dir = *optarg ? optarg : NULL; break;
		case 'E': cfg_flash_every = parse_uint(optarg, "flash interval", 300, 30 * 86400); break;
		case 's': cfg_socket = optarg; break;
		case 'u':
			if (nfwds == FWD_MAX || !fwd_parse_url(&fwds[nfwds], optarg)) {
				fprintf(stderr, "%s: %s\n", optarg, nfwds == FWD_MAX ? "at most 4 forward URLs" :
				        "not a usable http:// URL (https isn't supported)");
				return 2;
			}
			fwds[nfwds].fd.fd = fwds[nfwds].rfd.fd = -1;
			nfwds++;
			break;
		default: usage(argv[0]);
		}
	}
	if (optind != argc)
		usage(argv[0]);
	if (cfg_station && inet_pton(AF_INET, cfg_station, &station_addr) != 1) {
		fprintf(stderr, "station address must be an IPv4 address: %s\n", cfg_station);
		return 2;
	}
	if (coarse.interval && !coarse.state && fine.state &&
	    snprintf(coarse_default, sizeof(coarse_default), "%s.coarse", fine.state) < (int)sizeof(coarse_default))
		coarse.state = coarse_default;

	openlog("weatherd", LOG_PID | LOG_PERROR, LOG_DAEMON);
	fine.buf = calloc(fine.cap, sizeof(struct sample));
	coarse.buf = coarse.interval ? calloc(coarse.cap, sizeof(struct sample)) : NULL;
	if (!fine.buf || (coarse.interval && !coarse.buf)) {
		syslog(LOG_ERR, "cannot allocate history");
		return 1;
	}
	signal(SIGPIPE, SIG_IGN);
	if (cfg_flash_dir && (!coarse.interval || !flash_dir_ok())) {
		if (!coarse.interval)
			snprintf(flash_error, sizeof(flash_error), "needs the coarse tier; disabled");
		syslog(LOG_WARNING, "flash history: %s", flash_error);
		cfg_flash_dir = NULL;
	}
	state_load();

	uloop_init();
	listen_fd.fd = usock(USOCK_TCP | USOCK_SERVER | USOCK_IPV4ONLY | USOCK_NONBLOCK, NULL, cfg_port);
	if (listen_fd.fd < 0) {
		syslog(LOG_ERR, "cannot listen on port %s: %s", cfg_port, strerror(errno));
		return 1;
	}
	fcntl(listen_fd.fd, F_SETFD, FD_CLOEXEC);
	listen_fd.cb = listen_cb;
	uloop_fd_add(&listen_fd, ULOOP_READ);
	conn.path = cfg_socket;
	conn.cb = ubus_connected;
	ubus_auto_connect(&conn);
	if (cfg_flash_dir) {
		flash_timer.cb = flash_timer_cb;
		uloop_timeout_set(&flash_timer, cfg_flash_every * 1000);
	}
	if ((fine.state || coarse.state) && cfg_save_every) {
		save_timer.cb = save_timer_cb;
		uloop_timeout_set(&save_timer, cfg_save_every * 1000);
	}
	{
		unsigned i;

		for (i = 0; i < nfwds; i++)
			syslog(LOG_INFO, "repeating uploads to %s", fwds[i].url);
	}
	syslog(LOG_INFO, "listening on port %s for %s (passkey %s); keeping %u uploads, %u summaries of %us",
	       cfg_port, cfg_station ? cfg_station : "any address", cfg_passkey ? "checked" : "not checked",
	       fine.cap, coarse.interval ? coarse.cap : 0, coarse.interval);

	uloop_run();  /* until SIGTERM/SIGINT */

	flash_flush();
	state_save();
	if (ubus_ever_connected)  /* the context is only valid once connected */
		ubus_shutdown(&conn.ctx);
	uloop_done();
	free(fine.buf);
	free(coarse.buf);
	return 0;
}
