# huawei-trafficd: LTE usage, signal, latency and connection history on OpenWrt

A small daemon for the TP-Link TL-WR841N v9 (OpenWrt 15.05.1, `192.168.0.254`). Once a minute it samples the traffic counters, signal quality and connection state of the Huawei B310s-22 LTE router (`192.168.0.1`), and the internet latency and packet loss behind it, and keeps a timestamped history in two tiers: every sample for the last 3 days, and one sample per hour for the last year. You can query the history over HTTP from anywhere on the LAN, or with `ubus` on the router.

```text
Huawei 192.168.0.1 ──HTTP (anonymous)──▶ huawei-trafficd ──ubus──▶ uhttpd /ubus ──▶ LAN clients
                                          (RAM ring buffers)     └──▶ ubus call (on router)
```

- **Size:** a 33 KiB stripped binary. It links only to libraries already on the router (`libubus`, `libubox`, uClibc and `libgcc_s`), and needs no extra packages.
- **Memory:** about 810 KiB RSS measured on the router with a small history (much of it shared library pages). Each sample takes 64 bytes, so the full default history adds about 820 KiB: 270 KiB for the 3-day tier plus 548 KiB for the 1-year tier. That RAM is only used as the history fills.
- **No credentials:** the Huawei API returns these counters to an anonymous session. The daemon fetches a session cookie from `/api/webserver/SesTokInfo`, then reads `/api/monitoring/traffic-statistics`, `/api/device/signal` and `/api/monitoring/status` (the last two can be turned off with `signal 0`). It reuses the cookie and gets a new one only when the Huawei rejects it. If only the signal or status request fails, the traffic sample is still kept.
- **Latency:** one ICMP echo every 10 s, alternating between `1.1.1.1` and `8.8.8.8` (configurable), on a raw socket in the same event loop. A probe with no reply within 3 s counts as lost. That's about 25–30 MB of LTE data a month; it can be turned off with `latency 0`.
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
| `total_download`, `total_upload` | Huawei's cumulative counters (`TotalDownload`, `TotalUpload`). Not truly lifetime: see `--usage` below. |
| `rsrp` | Reference signal received power, dBm: how strong the cell's signal is |
| `rsrq` | Reference signal received quality, dB |
| `sinr` | Signal to interference plus noise ratio, dB: the main driver of speed |
| `rssi` | Received signal strength indicator, dBm (all power on the channel) |
| `signal_samples` | Readings behind the signal values: 1 for a fine sample, the hour's count for a coarse one |
| `cell_id` | LTE cell (ECI). `cell_id / 256` is the eNodeB (cell tower site) and `cell_id % 256` the sector |
| `conn_status` | Huawei `ConnectionStatus`: 900 connecting, 901 connected, 902 disconnected, 903 disconnecting |
| `net_type` | Huawei `CurrentNetworkType`: 19 is LTE; 1–3 are 2G and 4–18 or 41–46 are 3G |
| `ping_sent`, `ping_lost` | Latency probes that finished since the previous sample, and how many got no reply |
| `ping_ok` | Bitmask of targets that answered at least once (bit 0 = first target). `0` with `ping_sent > 0` means **no target answered**: an internet outage if the Huawei itself is still answering |
| `rtt_min`, `rtt_avg`, `rtt_max` | Round-trip time in ms over the replies; omitted if there were none |

The signal and connection fields are omitted when they weren't read (with `signal 0`, if that request failed, or in history from before this feature). The same goes for the latency fields: with `latency 0`, in a sample taken before any probe finished, or in older history.

Latency belongs to the samples, so while the Huawei can't be read (no sample is recorded), probe results build up and are attached to the next sample that is.

### History tiers

| Tier | Keeps | Default size | RAM when full |
|---|---|---|---|
| `fine` | Every sample (`interval`, 60 s) | 4320 samples = 3 days | ~236 KiB |
| `coarse` | The first sample of each `coarse_interval` (1 h, aligned to the clock hour) | 8760 samples = 365 days | ~479 KiB |

Each tier is a ring buffer, so the oldest samples are dropped first. Coarse samples are ordinary samples (taken at e.g. 15:00:37, not exactly on the hour). All counters are cumulative, so usage between any two samples is exact; the coarse tier only loses detail *within* each hour.

A single reading would say little about a whole hour, so when an hour closes, its coarse sample's signal values are replaced by the **average of that hour's readings** (`signal_samples` says how many). Its `conn_status` and `net_type` keep any non-connected status or different network type seen during the hour, so a brief disconnect or drop to 3G stays visible in the yearly history. Its latency fields hold the hour's totals (probes sent and lost), the bitwise OR of `ping_ok`, the reply-weighted average latency, and the hour's fastest and slowest reply.

The Huawei's own clock is not set (it reports 1970), so all timestamps come from OpenWrt. If the OpenWrt clock looks unset (before 2020), sampling is skipped and `status` reports why.

## Querying

### ubus methods (object `huawei.traffic`)

| Method | Arguments | Returns |
|---|---|---|
| `latest` | none | `{"sample": {...}}`; ubus status 9 (no data) until the first sample |
| `query` | `from`, `to`, `limit` (optional integers), `tier` (optional string) | `{"samples": [...], "count": n, "truncated": bool}` |
| `status` | none | per tier: interval, capacity, sample count, oldest/newest time, state file; plus last error, consecutive failures, whether `signal` collection is on, the last `signal_error`, and `latency` (enabled, interval, targets) |

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
./query.py --from 7d --events                # connection log (add --no-cells to skip cell changes); includes internet outages
./query.py --status
```

The table and CSV include the signal and connection fields. `--events` prints the connection log, worked out from consecutive samples exactly as the dashboard does (see below).

`--usage` (and the dashboard) add up the bytes between consecutive samples. Within one LTE session the session counters are exact. Across a reconnect they use the Huawei's totals, unless the new session's own counter is larger. That happens after a **Huawei crash or reboot**, which rolls its "lifetime" totals back to the last saved value: on 2026-11-15 a reboot dropped a 5½-day, 58 GB session from them. Traffic between the last sample and such a crash can't be recovered, so the result is then a lower bound.

### Web page

`www/traffic.html` is a dashboard served by the router itself at **http://192.168.0.254/traffic.html**. It is a single self-contained file (45 KiB, about 18 KiB on flash after JFFS2 compression) with no external scripts or fonts, so it works without internet access. It reads the same anonymous `/ubus` methods.

- **Status line:** whether collection is working, the last sample time, and the last error if collection is failing.
- **Current session:** uptime, session download/upload with the Huawei's totals, **speed now** (average download and upload over the last minute, from the two newest per-minute samples, counted per LTE session; "–" if they're more than 3 minutes apart), **internet now** (latest latency, and OK / Lossy / Down with a coloured dot), and **signal now**: a quality label (good / fair / poor, from SINR) with a colored dot, the SINR and RSRP values, and the cell.
- **Time range:** presets for 1h, 6h, 24h, 3d, 7d, 30d and 1y, or a custom from/to. The download and upload totals for the range are counted per LTE session, as described for `--usage` above.
- **Charts:** download and upload per period, as two charts with their own scales. The period grows with the range: from 1 minute (for 1h) to 1 week (for 1y). It also grows on narrow screens so each bar stays wide enough to read. Days and weeks follow your local calendar, including daylight-saving changes.
- **Signal charts:** average SINR and RSRP per period as two line charts, each with its own axis marked with quality zones (SINR: below 0 dB poor, 0–13 fair, 13–20 good, 20+ excellent; RSRP: below −100 dBm poor, −100 to −90 fair, −90 to −80 good, −80+ excellent). Periods without readings break the line. All four charts share the same x positions.
- **Latency charts:** average latency per period as a line, with the period's fastest-to-slowest range as a light band, plus packet loss as bars on a fixed 0–100 % axis. So that one extreme spike can't flatten the line, the latency axis is capped at 3× the highest average; the tooltip shows the true extremes. These use the same aqua as the signal charts, as one "link quality" colour: a fourth hue can't be told apart reliably from the other three (checked with the palette validator).
- **Connection log:** worked out from consecutive samples:
  - **Internet unreachable:** runs of samples where no latency target answered, with their duration.
  - **New LTE session:** the Huawei's session uptime restarted. The start time is exact even from hourly data, because it is the sample time minus the uptime.
  - **Collection gap:** no samples for more than 3 sampling intervals.
  - **Status changes:** connected or not connected.
  - **Network type changes:** for example LTE to 3G.
  - **Cell changes:** shown as eNodeB/sector. These can be frequent, so they are listed only when there are 20 or fewer in the range, or when "Show cell changes" is ticked. The summary line always counts them.
- **Details:** hovering a chart shows the period, bytes, average rate and average signal for that period. You can also focus a chart and use ← →, Home, End and Esc. "Show table" lists every period, including average SINR and RSRP, and "Download CSV" exports the raw samples in the range with every field.
- **Gaps:** where the daemon wasn't collecting, the bytes across the gap are known in total but not their timing. Those bars are faded, and their tooltip says how long the gap was.
- **Empty periods:** periods before the history starts, or with no samples yet, show no bar, as opposed to a zero bar.
- **Refresh and theme:** preset ranges refresh every minute. Light and dark mode follow the device setting.

Long ranges read the hourly tier when it reaches back far enough, in pages of 1000 samples to keep each router reply small (a full year is about 9 requests).

## Configuration

`/etc/config/huawei-trafficd`:

| Option | Default | Notes |
|---|---|---|
| `host`, `port` | `192.168.0.1`, `80` | Huawei address (numeric IPv4) |
| `signal` | `1` | Also read signal quality and connection status (two more anonymous requests per sample). `0` collects traffic counters only. |
| `latency` | `1` | Measure internet latency and packet loss (one ping every 10 s). `0` turns it off |
| `target` | `1.1.1.1`, `8.8.8.8` | Latency targets (UCI list, up to 4, IPv4 addresses), pinged in turn |
| `interval` | `60` | Seconds between samples (5–86400) |
| `history` | `4320` | Fine-tier samples kept in RAM |
| `coarse_interval` | `3600` | Seconds per coarse sample; must be longer than `interval`. `0` disables the coarse tier. |
| `coarse_history` | `8760` | Coarse-tier samples kept in RAM |
| `state_file` | `/tmp/huawei-trafficd.dat` | Fine history, loaded at start and written on stop. `/tmp` survives a service restart but **not a reboot**. |
| `coarse_state_file` | `<state_file>.coarse` | Coarse history file |
| `save_every` | `0` | Also write the history files every N seconds (minimum 60) |
| `flash_dir` | `/srv/huawei-trafficd` | Keep the hourly history on flash as well, so it survives reboots and power cuts (see below). Empty turns it off. |
| `flash_every` | `86400` | Seconds between appends to the flash history (300 to 30 days) |

Apply changes with `/etc/init.d/huawei-trafficd restart`.

State files from earlier versions still load. A one-tier file becomes the fine tier, and the coarse tier is rebuilt from it. Samples from before signal collection have no signal fields.

### Keeping history across reboots

The `/tmp` files are RAM, so a reboot or power cut loses them. To keep the **hourly history** anyway, the daemon also appends it to flash in `flash_dir` (on by default):

- **What's written:** only finished hourly samples (the still-open hour stays in RAM), compressed to about 18 bytes each. Each sample stores only the differences from the previous one, as variable-length numbers. One file per month (UTC): `YYYY-MM.log`.
- **When:** every `flash_every` seconds (default once a day) and on a clean stop of the service. A power cut loses at most that interval of hourly data, plus the per-minute tier and the open hour, which live only in RAM.
- **How much:** about 13 KiB a month. The **13 newest monthly files** are kept, about 160 KiB in total; older months are deleted as whole files, so nothing is ever rewritten. If the overlay has less than 64 KiB free, the daemon stops writing and reports it in `status` instead of filling the flash.
- **Wear:** each append is one small write of about 0.5 KB a day, and JFFS2 spreads erases over the 9 blocks of the 576 KiB overlay. Including JFFS2's own copying, that's at most about 2 erases per block per year, against roughly 100,000 rated cycles for this NOR flash (MX25L3205D).
- **Robustness:** each append is one block with a CRC-32. At startup, a block damaged inside is skipped, and a torn or garbled end of a file (for example from a power cut mid-write) is cut off so later appends stay readable.
- **Startup:** hourly samples are rebuilt from these files, plus anything newer from `/tmp` after a plain service restart.
- **Must be on flash:** a directory in RAM (`/tmp`, `/var`) is refused with a warning, because it would silently not persist.
- **`status`** has a `flash` section: directory, interval, number of files, bytes, last saved sample and any error.

**Format** (little-endian), for reading the files elsewhere:
- **Block:** a 12-byte header (`"HB"`, version `1`, record count, payload length as u16, 2 zero bytes, CRC-32 of the payload as u32), then the records. The first record of each block is encoded against an all-zero sample, so every block decodes on its own.
- **Record:**
  - a flags byte;
  - zigzag varints for: time delta, uptime drift (uptime delta minus time delta), session download and upload deltas, and deltas of total-minus-session for download and upload;
  - if flag 1 (signal): bytes `rsrp+140`, `rsrq+40`, `sinr+40`, `rssi+140` (0 = unknown), then the reading count;
  - if flag 2 (cell, status or network type changed): varints for the cell ID and the status, then a byte for the network type;
  - if flag 4 (latency): varints for probes sent, probes lost, RTT min, RTT avg and RTT max (ms), then a byte for the `ping_ok` mask.

  Records written before latency existed simply lack flag 4.

**Turning it off:** set `option flash_dir ''`, run `/etc/init.d/huawei-trafficd restart`, then `rm -r /srv/huawei-trafficd` to free the space.

**Firmware upgrades:** a sysupgrade that keeps settings doesn't keep `/srv`. If you want the history kept, add `/srv/huawei-trafficd/` to `/etc/sysupgrade.conf` first.

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
rm -r /srv/huawei-trafficd   # hourly history on flash
/etc/init.d/rpcd restart
```

## Verification performed (2026-11-15)

- Under qemu on the build host, the MIPS binary fetched real counters from the Huawei and saved them on SIGTERM.
- A native build was run against host builds of libubox and ubus at the router's exact commits. That test covered every method, ring-buffer eviction, relative `from`, `limit`/`truncated`, invalid arguments, saving and reloading the history, and errors for a refused connection and for a timeout.
- **Two-tier update:** the native build was tested with a 5-second sampling interval and a 12-second coarse bucket, with small capacities. That covered bucket boundaries, eviction in both tiers, the `auto` merge at the tier boundary, `tier` selection and validation, pagination across tiers, saving and reloading both files, and migrating a version-1 file (10 half-hourly samples became 5 hourly samples). The update was installed on the router by replacing the binary, init script and config. The existing history carried over, overlay use was 268 KiB afterwards, and RSS was 812 KiB.
- **Web page:** rendered in Chromium against a mock server holding a year of synthetic data, including a 6-hour collection gap. Every preset was checked in light and dark mode, plus a custom range, a 390 px phone layout (no sideways scrolling), keyboard navigation, the table, and the Europe/Lisbon daylight-saving boundary for week buckets. It was then installed at `/www/traffic.html` and loaded from the router with real data, with no console errors.
- **Latency update (2026-11-16):** tested in network namespaces, with a test build pinging every second and the targets and mock Huawei in the other namespace.
  - **Conditions** injected with `tc netem` and `nft`:
    - 40 ms delay was measured as 40–45 ms;
    - 50 % loss was counted;
    - dropping only ICMP (Huawei still answering) gave consecutive outage samples, then recovery;
    - one target down gave mask 1, not an outage.
  - **Hourly totals and extremes** were correct.
  - **Migration:** a v3 state file written by the previous version loaded and was rewritten as v4.
  - **Flash:** hourly latency fields survived a simulated reboot.
  - **Fuzzing:** 4,748 random ICMP packets, plus 10 s of forged echo replies during measurement, changed nothing. No sanitizer reports.
  - **On the router:** the 961 + 17 existing samples migrated, and live readings were about 44 ms average to 1.1.1.1 and 8.8.8.8 with no loss.
- **Signal and connection update:**
  - **Soak test:** the Huawei rebooted 3 times on 2026-11-15, shortly after a one-off probe of several of its API endpoints. The cause wasn't established, so before deploying, `device/signal` and `monitoring/status` were polled from the LAN once a minute for 20 minutes. The Huawei's session uptime rose steadily throughout, with no reboots or reconnects.
  - **Native test:** a native build ran against a mock Huawei that could fail each endpoint on demand (error, garbage or timeout). In every case the traffic sample was kept and `signal_error` was reported.
  - **Also checked:** hourly averaging (6 readings of −100/−110 dBm averaged to −107), status, network-type and cell changes, uptime resets, loading old-format files, and `-G` sending only the two traffic requests.
  - **Dashboard:** rendered against a year of mock data with a reboot, a disconnect, a 3G spell, cell changes and a gap, in light, dark and phone layouts.
  - **On the router:** the upgrade loaded the existing 225 + 4 samples. The connection log shows the three real Huawei reboots of 15:34–15:41.
- **Flash history:** tested with a native build against the mock Huawei, using a 12-second "hour":
  - **Restores:** after a clean stop and a simulated reboot (`/tmp` files deleted), every finished hourly sample came back identical in all fields. A plain service restart gave no duplicates.
  - **Damage:** a torn last block is cut off, a block with a flipped byte is skipped (the blocks before and after it still load), and garbage at the end is trimmed.
  - **Pruning:** months older than the newest 13 are deleted, and other files are left alone.
  - **Safety:** a directory in RAM is refused.
  - **Size:** about 20 bytes per record in a 14-record block.
  - **On the router:** the first file written on the router was decoded independently in Python and matched the daemon's hourly samples exactly. The test also found a bug: a network type seen briefly within an hour (for example 3G) wasn't kept in the hourly sample. It's fixed.
- Installed on the router as above (overlay use went from 284 to 292 KiB). The service is enabled at boot and running. `ubus call` and anonymous HTTP `latest`/`query` work, while anonymous `system.info` is still `Access denied`. History survived `/etc/init.d/huawei-trafficd restart`.

## Security notes

Anyone on the LAN can read the usage history (no login), which is the intended behaviour. They cannot change anything: the ACL grants only `latest`, `query` and `status`, none of which modify state. The daemon makes outbound plain-HTTP requests to the Huawei only. It parses fixed XML tags into fixed-size buffers and rejects HTTP responses larger than 4 KiB.
