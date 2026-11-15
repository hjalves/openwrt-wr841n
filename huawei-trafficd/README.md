# huawei-trafficd: LTE data usage history on OpenWrt

A small daemon for the TP-Link TL-WR841N v9 (OpenWrt 15.05.1, `192.168.0.254`). Once a minute it samples the traffic counters of the Huawei B310s-22 LTE router (`192.168.0.1`) and keeps a timestamped history in two tiers: every sample for the last 3 days, and one sample per hour for the last year. You can query the history over HTTP from anywhere on the LAN, or with `ubus` on the router.

```text
Huawei 192.168.0.1 ──HTTP (anonymous)──▶ huawei-trafficd ──ubus──▶ uhttpd /ubus ──▶ LAN clients
                                          (RAM ring buffers)     └──▶ ubus call (on router)
```

- **Size:** a 19 KiB stripped binary. It links only to libraries already on the router (`libubus`, `libubox`, uClibc and `libgcc_s`), and needs no extra packages.
- **Memory:** about 810 KiB RSS measured on the router with a small history (much of it shared library pages). Each sample takes 40 bytes, so the full default history adds about 510 KiB: 170 KiB for the 3-day tier plus 340 KiB for the 1-year tier. That RAM is only used as the history fills.
- **No credentials:** the Huawei API returns these counters to an anonymous session. The daemon fetches a session cookie from `/api/webserver/SesTokInfo`, then reads `/api/monitoring/traffic-statistics`. It reuses the cookie and gets a new one only when the Huawei rejects it.
- **Non-blocking:** HTTP collection runs in the same `uloop` as ubus, with a 5 s timeout, so queries are answered even when the Huawei is unreachable.

## Why ubus (through uhttpd) instead of a separate HTTP server

The router already runs uhttpd with the ubus JSON-RPC bridge and rpcd for access control. Registering a ubus object therefore means the daemon has no HTTP server, socket listener or JSON encoder of its own. That keeps it small, avoids opening another port, and lets you also use `ubus call` locally. The cost is that HTTP clients have to send a JSON-RPC POST instead of a plain `GET`. A small rpcd ACL allows **anonymous** read access to just this object's three read-only methods; everything else on `/ubus` stays protected.

## Data

Each sample:

| Field | Meaning |
|---|---|
| `time` | Unix timestamp from the OpenWrt router's clock (NTP-synchronized) |
| `uptime` | Seconds since the current LTE session started (`CurrentConnectTime`) |
| `download`, `upload` | Bytes in the current session (`CurrentDownload`, `CurrentUpload`). These reset when the LTE session reconnects. |
| `total_download`, `total_upload` | Huawei's cumulative counters (`TotalDownload`, `TotalUpload`) |

### History tiers

| Tier | Keeps | Default size | RAM when full |
|---|---|---|---|
| `fine` | Every sample (`interval`, 60 s) | 4320 samples = 3 days | ~170 KiB |
| `coarse` | The first sample of each `coarse_interval` (1 h, aligned to the clock hour) | 8760 samples = 365 days | ~340 KiB |

Each tier is a ring buffer, so the oldest samples are dropped first. Coarse samples are ordinary samples (taken at e.g. 15:00:37, not exactly on the hour). All counters are cumulative, so usage between any two samples is exact; the coarse tier only loses detail *within* each hour.

The Huawei's own clock is not set (it reports 1970), so all timestamps come from OpenWrt. If the OpenWrt clock looks unset (before 2020), sampling is skipped and `status` reports why.

## Querying

### ubus methods (object `huawei.traffic`)

| Method | Arguments | Returns |
|---|---|---|
| `latest` | none | `{"sample": {...}}`; ubus status 9 (no data) until the first sample |
| `query` | `from`, `to`, `limit` (optional integers), `tier` (optional string) | `{"samples": [...], "count": n, "truncated": bool}` |
| `status` | none | per tier: interval, capacity, sample count, oldest/newest time, state file; plus last error and consecutive failures |

`from` and `to` are Unix timestamps (inclusive). A **negative** value is relative to now, so `{"from": -3600}` means the last hour. Samples are returned oldest first. `limit` defaults to 1000 and is capped at 4000 to keep replies under ubus's 1 MiB message limit. If `truncated` is true, ask again with `from` set to the last returned `time + 1`.

`tier` selects the history:

- `auto` (default): coarse samples older than the oldest fine sample, followed by all fine samples. This gives one continuous series: hourly for the past year, then every minute for the last 3 days.
- `fine`: only the per-minute tier.
- `coarse`: only the hourly tier, including recent hours. Use this for an evenly spaced hourly series.

An unknown `tier` returns ubus status 2 (invalid argument).

On the router:

```sh
ubus call huawei.traffic latest
ubus call huawei.traffic query '{"from": -86400}'
ubus call huawei.traffic query '{"from": -2592000, "tier": "coarse"}'   # hourly, 30 days
ubus call huawei.traffic status
```

Over HTTP from the LAN (`00000000000000000000000000000000` is the anonymous session):

```sh
curl -s http://192.168.0.254/ubus -H 'Content-Type: application/json' --data \
 '{"jsonrpc":"2.0","id":1,"method":"call","params":["00000000000000000000000000000000","huawei.traffic","query",{"from":-3600}]}'
```

Reply:

```json
{"jsonrpc":"2.0","id":1,"result":[0,{"samples":[{"time":1794751857,"uptime":472992,"download":57906808352,"upload":6885072686,"total_download":4232562419036,"total_upload":224152610467}],"count":1,"truncated":false}]}
```

`result[0]` is the ubus status (`0` = success).

### `query.py` (host-side helper, Python 3 standard library only)

```sh
./query.py                                   # latest sample
./query.py --from 1h                         # last hour, table with local times
./query.py --from 2026-11-15T08:00 --to 2026-11-15T12:00 --format csv
./query.py --from 1d --usage                 # bytes transferred in the last day
./query.py --from 30d --tier coarse --format csv   # hourly samples for 30 days
./query.py --status
```

`--usage` adds up the differences between consecutive samples using the Huawei's lifetime totals, which keep counting across LTE reconnects. This makes it exact even across hourly gaps that contain a reconnect. If a total ever goes backwards (the counters were cleared on the Huawei), it falls back to the session counters for that step.

### Web page

`www/traffic.html` is a dashboard served by the router itself at **http://192.168.0.254/traffic.html**. It is a single self-contained file (26 KiB, about 8 KiB on flash after JFFS2 compression) with no external scripts or fonts, so it works without internet access. It reads the same anonymous `/ubus` methods.

- **Status line:** whether collection is working, the last sample time, and the last error if collection is failing.
- **Current session:** uptime, and session download/upload with the lifetime totals.
- **Time range:** presets for 1h, 6h, 24h, 3d, 7d, 30d and 1y, or a custom from/to. The download and upload totals for the range use lifetime counters, so they stay exact across reconnects.
- **Charts:** download and upload per period, as two charts with their own scales. The period grows with the range: from 1 minute (for 1h) to 1 week (for 1y). It also grows on narrow screens so each bar stays wide enough to read. Days and weeks follow your local calendar, including daylight-saving changes.
- **Details:** hovering a bar shows the period, bytes and average rate. You can also focus a chart and use ← →, Home, End and Esc. "Show table" lists every period, and "Download CSV" exports the raw samples in the range.
- **Gaps:** where the daemon wasn't collecting, the bytes across the gap are known in total but not their timing. Those bars are faded, and their tooltip says how long the gap was.
- **Empty periods:** periods before the history starts, or with no samples yet, show no bar, as opposed to a zero bar.
- **Refresh and theme:** preset ranges refresh every minute. Light and dark mode follow the device setting.

Long ranges read the hourly tier when it reaches back far enough, in pages of 1000 samples to keep each router reply small (a full year is about 9 requests).

## Configuration

`/etc/config/huawei-trafficd`:

| Option | Default | Notes |
|---|---|---|
| `host`, `port` | `192.168.0.1`, `80` | Huawei address (numeric IPv4) |
| `interval` | `60` | Seconds between samples (5–86400) |
| `history` | `4320` | Fine-tier samples kept in RAM |
| `coarse_interval` | `3600` | Seconds per coarse sample; must be longer than `interval`. `0` disables the coarse tier. |
| `coarse_history` | `8760` | Coarse-tier samples kept in RAM |
| `state_file` | `/tmp/huawei-trafficd.dat` | Fine history, loaded at start and written on stop. `/tmp` survives a service restart but **not a reboot**. |
| `coarse_state_file` | `<state_file>.coarse` | Coarse history file |
| `save_every` | `0` | Also write the history files every N seconds (minimum 60) |

Apply changes with `/etc/init.d/huawei-trafficd restart`.

State files written by the first version (one tier) still load: they become the fine tier, and the coarse tier is rebuilt from them.

### Keeping history across reboots

Both files are in `/tmp` by default, which is RAM, so **a reboot loses the history**. Saving to flash is limited by this router's small JFFS2 overlay (about 280–300 KiB free):

- A full year of coarse history is 342 KiB, which doesn't fit. JFFS2 compression might bring it to roughly 190–260 KiB (estimated on synthetic data). But the save writes a new file before replacing the old one, so it briefly needs room for two copies. **Don't point a full-year coarse tier at flash.**
- What fits safely: put `coarse_state_file` on flash with a shorter `coarse_history`, for example `2160` (90 days, 86 KiB), and set `save_every` to `3600`. JFFS2 spreads writes across the flash, so an hourly save is a modest amount of wear.
- For a full year that survives reboots, copy the history off the router regularly (for example a scheduled `query.py --tier coarse --format csv` on another machine).

Daemon options: `huawei-trafficd -h`. It logs to syslog (`logread | grep huawei-trafficd`), only when sampling starts failing and when it recovers.

## Build

```sh
make -C huawei-trafficd
```

This uses the OpenWrt 15.05.1 SDK in `../toolchain/`; see [Building for the router](../README.md#building-for-the-router) for the download link, SHA-256 and setup.

**On an ARM64 build host** (such as the Arch Linux ARM VM): the SDK's compiler is an x86-64 Linux program. It runs under `qemu-user` binfmt emulation, given an x86-64 glibc runtime:

```sh
sudo pacman -S qemu-user qemu-user-binfmt
# Extract x86_64 glibc, libgcc, libstdc++ and zlib packages into /opt/x86_64-root
# (with lib and lib64 symlinked to usr/lib), then:
QEMU_LD_PREFIX=/opt/x86_64-root make -C huawei-trafficd
```

Rebuilding an earlier example program this way reproduced its previously built binary byte for byte, so the emulated toolchain behaves like a native one.

## Install

Copy the files to the router's `/tmp` with the [`router-ssh`](../router-ssh/README.md) helpers, from the workspace root:

```sh
./router-ssh/connect 'mkdir -m 700 /tmp/htd-install'
./router-ssh/copy huawei-trafficd/huawei-trafficd huawei-trafficd/files/* huawei-trafficd/www/traffic.html router:/tmp/htd-install/
./router-ssh/connect
```

Then on the router, in `/tmp/htd-install`:

```sh
cp huawei-trafficd           /usr/sbin/huawei-trafficd && chmod 755 /usr/sbin/huawei-trafficd
cp huawei-trafficd.init      /etc/init.d/huawei-trafficd && chmod 755 /etc/init.d/huawei-trafficd
cp huawei-trafficd.config    /etc/config/huawei-trafficd
cp huawei-trafficd.acl.json  /usr/share/rpcd/acl.d/huawei-trafficd.json
cp traffic.html              /www/traffic.html          # optional web page
/etc/init.d/huawei-trafficd enable
/etc/init.d/huawei-trafficd start
/etc/init.d/rpcd restart     # the anonymous session's ACLs are loaded at rpcd start
```

Restarting rpcd logs out existing LuCI and ubus sessions. procd restarts the daemon if it crashes.

**Uninstall:**

```sh
/etc/init.d/huawei-trafficd stop; /etc/init.d/huawei-trafficd disable
rm /usr/sbin/huawei-trafficd /etc/init.d/huawei-trafficd /etc/config/huawei-trafficd \
   /usr/share/rpcd/acl.d/huawei-trafficd.json /www/traffic.html /tmp/huawei-trafficd.dat*
/etc/init.d/rpcd restart
```

## Verification performed (2026-11-15)

- Under qemu on the build host, the MIPS binary fetched real counters from the Huawei and saved them on SIGTERM.
- A native build was run against host builds of libubox and ubus at the router's exact commits. That test covered every method, ring-buffer eviction, relative `from`, `limit`/`truncated`, invalid arguments, saving and reloading the history, and errors for a refused connection and for a timeout.
- **Two-tier update:** the native build was tested with a 5-second sampling interval and a 12-second coarse bucket, with small capacities. That covered bucket boundaries, eviction in both tiers, the `auto` merge at the tier boundary, `tier` selection and validation, pagination across tiers, saving and reloading both files, and migrating a version-1 file (10 half-hourly samples became 5 hourly samples). The update was installed on the router by replacing the binary, init script and config. The existing history carried over, overlay use was 268 KiB afterwards, and RSS was 812 KiB.
- **Web page:** rendered in Chromium against a mock server holding a year of synthetic data, including a 6-hour collection gap. Every preset was checked in light and dark mode, plus a custom range, a 390 px phone layout (no sideways scrolling), keyboard navigation, the table, and the Europe/Lisbon daylight-saving boundary for week buckets. It was then installed at `/www/traffic.html` and loaded from the router with real data, with no console errors.
- Installed on the router as above (overlay use went from 284 to 292 KiB). The service is enabled at boot and running. `ubus call` and anonymous HTTP `latest`/`query` work, while anonymous `system.info` is still `Access denied`. History survived `/etc/init.d/huawei-trafficd restart`.

## Security notes

Anyone on the LAN can read the usage history (no login), which is the intended behaviour. They cannot change anything: the ACL grants only `latest`, `query` and `status`, none of which modify state. The daemon makes outbound plain-HTTP requests to the Huawei only. It parses fixed XML tags into fixed-size buffers and rejects HTTP responses larger than 4 KiB.
