# huawei-trafficd: LTE data usage history on OpenWrt

A small daemon for the TP-Link TL-WR841N v9 (OpenWrt 15.05.1, `192.168.0.254`). Once a minute it samples the traffic counters of the Huawei B310s-22 LTE router (`192.168.0.1`) and keeps a timestamped history. You can query the history over HTTP from anywhere on the LAN, or with `ubus` on the router.

```text
Huawei 192.168.0.1 ──HTTP (anonymous)──▶ huawei-trafficd ──ubus──▶ uhttpd /ubus ──▶ LAN clients
                                          (RAM ring buffer)     └──▶ ubus call (on router)
```

- **Size:** a 16 KiB stripped binary. It links only to libraries already on the router (`libubus`, `libubox`, uClibc and `libgcc_s`), and needs no extra packages.
- **Memory:** about 820 KiB RSS measured on the router (much of it shared library pages). The history uses 40 bytes per sample, so the default 3 days (4320 samples) is about 170 KiB.
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

The Huawei's own clock is not set (it reports 1970), so all timestamps come from OpenWrt. If the OpenWrt clock looks unset (before 2020), sampling is skipped and `status` reports why.

## Querying

### ubus methods (object `huawei.traffic`)

| Method | Arguments | Returns |
|---|---|---|
| `latest` | none | `{"sample": {...}}`; ubus status 9 (no data) until the first sample |
| `query` | `from`, `to`, `limit` (all optional integers) | `{"samples": [...], "count": n, "truncated": bool}` |
| `status` | none | interval, capacity, sample count, oldest/newest time, last error, consecutive failures |

`from` and `to` are Unix timestamps (inclusive). A **negative** value is relative to now, so `{"from": -3600}` means the last hour. Samples are returned oldest first. `limit` defaults to 1000 and is capped at 4000 to keep replies under ubus's 1 MiB message limit. If `truncated` is true, ask again with `from` set to the last returned `time + 1`.

On the router:

```sh
ubus call huawei.traffic latest
ubus call huawei.traffic query '{"from": -86400}'
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
./query.py --status
```

`--usage` adds up the differences between consecutive samples. It treats a drop in the session counters as a new LTE session, so a reconnect inside the range is still counted correctly.

## Configuration

`/etc/config/huawei-trafficd`:

| Option | Default | Notes |
|---|---|---|
| `host`, `port` | `192.168.0.1`, `80` | Huawei address (numeric IPv4) |
| `interval` | `60` | Seconds between samples (5–86400) |
| `history` | `4320` | Samples kept in RAM. The oldest are dropped first. |
| `state_file` | `/tmp/huawei-trafficd.dat` | Loaded at start and written on stop. `/tmp` survives a service restart but **not a reboot**. |
| `save_every` | `0` | Also write `state_file` every N seconds (minimum 60) |

To keep history across reboots, point `state_file` at flash (for example `/etc/huawei-trafficd.dat`, about 170 KiB when full) and set `save_every` to something like `3600`. Keep in mind this router has only about 280 KiB of free JFFS2 space and its flash wears with frequent writes. Apply changes with `/etc/init.d/huawei-trafficd restart`.

Daemon options: `huawei-trafficd -h`. It logs to syslog (`logread | grep huawei-trafficd`), only when sampling starts failing and when it recovers.

## Build

```sh
make -C huawei-trafficd
```

This uses the OpenWrt 15.05.1 SDK in `../toolchain/`, as described in [`../ubus-hello/README.md`](../ubus-hello/README.md) and [`../hello-http/README.md`](../hello-http/README.md) (download link and SHA-256).

**On an ARM64 build host** (such as the Arch Linux ARM VM): the SDK's compiler is an x86-64 Linux program. It runs under `qemu-user` binfmt emulation, given an x86-64 glibc runtime:

```sh
sudo pacman -S qemu-user qemu-user-binfmt
# Extract x86_64 glibc, libgcc, libstdc++ and zlib packages into /opt/x86_64-root
# (with lib and lib64 symlinked to usr/lib), then:
QEMU_LD_PREFIX=/opt/x86_64-root make -C huawei-trafficd
```

Rebuilding `ubus-hello` this way reproduced the committed binary, so the emulated toolchain behaves like a native one.

## Install

Copy the files to the router's `/tmp` with the [`router-ssh`](../router-ssh/README.md) helpers, from the workspace root:

```sh
./router-ssh/connect 'mkdir -m 700 /tmp/htd-install'
./router-ssh/copy huawei-trafficd/huawei-trafficd huawei-trafficd/files/* router:/tmp/htd-install/
./router-ssh/connect
```

Then on the router, in `/tmp/htd-install`:

```sh
cp huawei-trafficd           /usr/sbin/huawei-trafficd && chmod 755 /usr/sbin/huawei-trafficd
cp huawei-trafficd.init      /etc/init.d/huawei-trafficd && chmod 755 /etc/init.d/huawei-trafficd
cp huawei-trafficd.config    /etc/config/huawei-trafficd
cp huawei-trafficd.acl.json  /usr/share/rpcd/acl.d/huawei-trafficd.json
/etc/init.d/huawei-trafficd enable
/etc/init.d/huawei-trafficd start
/etc/init.d/rpcd restart     # the anonymous session's ACLs are loaded at rpcd start
```

Restarting rpcd logs out existing LuCI and ubus sessions. procd restarts the daemon if it crashes.

**Uninstall:**

```sh
/etc/init.d/huawei-trafficd stop; /etc/init.d/huawei-trafficd disable
rm /usr/sbin/huawei-trafficd /etc/init.d/huawei-trafficd /etc/config/huawei-trafficd \
   /usr/share/rpcd/acl.d/huawei-trafficd.json /tmp/huawei-trafficd.dat
/etc/init.d/rpcd restart
```

## Verification performed (2026-11-15)

- Under qemu on the build host, the MIPS binary fetched real counters from the Huawei and saved them on SIGTERM.
- A native build was run against host builds of libubox and ubus at the router's exact commits. That test covered every method, ring-buffer eviction, relative `from`, `limit`/`truncated`, invalid arguments, saving and reloading the history, and errors for a refused connection and for a timeout.
- Installed on the router as above (overlay use went from 284 to 292 KiB). The service is enabled at boot and running. `ubus call` and anonymous HTTP `latest`/`query` work, while anonymous `system.info` is still `Access denied`. History survived `/etc/init.d/huawei-trafficd restart`.

## Security notes

Anyone on the LAN can read the usage history (no login), which is the intended behaviour. They cannot change anything: the ACL grants only `latest`, `query` and `status`, none of which modify state. The daemon makes outbound plain-HTTP requests to the Huawei only. It parses fixed XML tags into fixed-size buffers and rejects HTTP responses larger than 4 KiB.
