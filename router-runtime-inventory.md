# Router development runtime inventory

Read-only SSH inventory on 2026-11-14, after the DDNS packages were removed. This describes what was found on this particular router, not everything OpenWrt can support. Process and listener states are snapshots. [The raw inventory](router-runtime-raw.md) contains every discovered shared-object path and symlink, Lua file, BusyBox applet, executable-directory listing, and installed package/version.

## Platform and build constraints

- 32-bit, big-endian MIPS; MIPS32r2, 24Kc CPU; Linux 3.18.23.
- OpenWrt 15.05.1 `ar71xx/generic` uses **uClibc 0.9.33.2**, not glibc or musl. Its dynamic loader is `/lib/ld-uClibc.so.0`.
- No `gcc`, `cc`, `g++`, `make`, `ld`, `ldd`, `readelf`, or `objdump` command was found on the router's PATH, and no C headers or static archives were found in the checked standard library/include directories. Cross-compile on another computer with a matching SDK.
- The [tested example](hello-http/README.md) uses the OpenWrt 15.05.1 ar71xx SDK and links dynamically against the libraries already on the router. A generic glibc-based MIPS build failed with an illegal instruction; matching the CPU and ABI matters.
- Shared library presence does not imply every feature, header, or API of a newer version is present. Names without version numbers are the observed files, not necessarily development linker symlinks. Sizes and complete symlink targets are in the raw inventory.

## Installed C shared libraries

### C runtime and basic support

| Router path | Purpose |
|---|---|
| `/lib/ld-uClibc-0.9.33.2.so`, alias `/lib/ld-uClibc.so.0` | ELF dynamic loader |
| `/lib/libuClibc-0.9.33.2.so`, alias `/lib/libc.so.0` | C runtime, system calls, sockets and standard C library |
| `/lib/libcrypt-0.9.33.2.so`, alias `/lib/libcrypt.so.0` | Password-hashing/crypt routines; not a general TLS library |
| `/lib/libdl-0.9.33.2.so`, alias `/lib/libdl.so.0` | Dynamic loading |
| `/lib/libm-0.9.33.2.so`, alias `/lib/libm.so.0` | Math routines |
| `/lib/libutil-0.9.33.2.so`, alias `/lib/libutil.so.0` | Unix utility routines |
| `/lib/libgcc_s.so.1` | GCC runtime support |

No separate `libpthread`, OpenSSL `libssl`/`libcrypto`, zlib `libz`, SQLite, or C++ `libstdc++` shared library was found in the inspected standard library paths. That is not a proof that every related operation is impossible, but do not assume those libraries are available to link against.

### POSIX threads: tested and working

The absence of a preinstalled `libpthread` does **not** mean the router cannot use threads. A test compiled with the matching OpenWrt SDK successfully called `pthread_create` and `pthread_join` on the router, with a 64 KiB worker-thread stack. Both tests were run from `/tmp` and cleaned up afterward:

1. A fully static uClibc/pthread test executable was **120,608 bytes** and required no separately installed pthread library.
2. A dynamically linked test executable was **4,964 bytes**. A stripped matching SDK `libpthread.so.0` was **87,524 bytes**; supplying it in a temporary directory via `LD_LIBRARY_PATH` also worked. This shared library is not installed on the router by default.

For the static test, the old SDK required explicitly grouping `-lpthread -lc -lgcc -lgcc_eh`; a plain `-static -pthread` link alone failed with unresolved unwind symbols. The successful command shape was:

```sh
mips-openwrt-linux-uclibc-gcc -Os -mips32r2 -mtune=24kc -msoft-float \
  -static -pthread -s -Wl,-z,noexecstack thread-test.c \
  -Wl,--start-group -lpthread -lc -lgcc -lgcc_eh -Wl,--end-group \
  -o thread-test
```

This verifies one create/join test, not production stability under load. Thread stacks and other per-thread memory consume the router's limited RAM. For a long-running service, measure actual peak memory, set reasonable stack sizes where appropriate, and leave headroom for the router's existing services.

### OpenWrt, networking, JSON, and Lua

| Router path | Purpose |
|---|---|
| `/lib/libblobmsg_json.so` | OpenWrt blob-message / JSON conversion |
| `/lib/libfstools.so` | OpenWrt filesystem-management support |
| `/lib/libjson_script.so` | OpenWrt JSON script support |
| `/lib/libsetlbf.so` | OpenWrt buffering support library |
| `/lib/libubox.so` | OpenWrt utility/event-loop/blob infrastructure |
| `/lib/libubus.so` | OpenWrt local IPC bus client library |
| `/lib/libuci.so` | OpenWrt configuration interface |
| `/lib/libvalidate.so` | OpenWrt data validation support |
| `/usr/lib/libiwinfo.so` | Wireless information |
| `/usr/lib/libjson-c.so.2.0.1`, alias `.so.2` | JSON-C; installed package version 0.12 |
| `/usr/lib/liblua.so.5.1.5` | Lua 5.1 runtime library |
| `/usr/lib/libnl-tiny.so` | Lightweight netlink library |
| `/usr/lib/libip4tc.so.0.1.0`, aliases `.so.0` and `.so` | IPv4 iptables support |
| `/usr/lib/libip6tc.so.0.1.0`, aliases `.so.0` and `.so` | IPv6 iptables support |
| `/usr/lib/libxtables.so.10.0.0`, aliases `.so.10` and `.so` | iptables/xtables support |
| `/usr/lib/libiptext.so`, `/usr/lib/libiptext4.so`, `/usr/lib/libiptext6.so` | iptables extension support |

### Other shared objects

| Router path | Purpose |
|---|---|
| `/usr/lib/uhttpd_ubus.so` | uhttpd's ubus integration plugin |
| `/usr/lib/pppd/2.4.7/rp-pppoe.so` | PPPoE plugin for pppd |
| `/usr/lib/lua/iwinfo.so` | Lua wireless information module |
| `/usr/lib/lua/luci/ip.so` | Lua/LuCI IP address module |
| `/usr/lib/lua/luci/template/parser.so` | LuCI template parser |
| `/usr/lib/lua/nixio.so` | Lua POSIX/networking module |
| `/usr/lib/lua/ubus.so` | Lua ubus binding |
| `/usr/lib/lua/uci.so` | Lua UCI binding |

Kernel modules such as `kmod-crypto-aes` also exist, but kernel crypto modules are not interchangeable with user-space OpenSSL or another application library.

## Interpreters and scripting

| Interpreter/tool | Observed availability |
|---|---|
| `/usr/bin/lua` | **Lua 5.1.5**; version banner reports `double int32` |
| `/bin/sh`, `/bin/ash` | BusyBox **ash**, BusyBox version 1.23.2 |
| `/usr/bin/awk` | BusyBox awk |
| `/bin/sed` | BusyBox sed |
| `/usr/bin/expr` | BusyBox expression tool |
| `/usr/bin/jsonfilter`, `/usr/bin/jshn` | JSON querying and OpenWrt shell JSON helper executable |
| `/usr/share/libubox/jshn.sh` | Shell JSON helper functions |

No `python`, `python2`, `python3`, `perl`, `ruby`, `php`, `node`, `nodejs`, `ucode`, `bash`, or `luac` command was found on PATH. This search does not prove an executable could not exist under an unexpected name elsewhere.

### Lua modules tested

The following `require()` calls succeeded: `nixio`, `nixio.fs`, `nixio.util`, `uci`, `ubus`, `iwinfo`, `luci.http`, `luci.model.uci`, `luci.ip`, `luci.ltn12`, and `luci.template.parser`. `nixio` exposed `socket`, `connect`, `bind`, `poll`, `gettimeofday`, `fork`, and `exec` functions. This is an availability check, not a full functional or security test.

`require()` did not successfully load `socket`, `ssl`, `lfs`, `cjson`, `luci.json`, or `luci.jsonc`; no LuaSocket or LuaSec module was identified. The router has many more LuCI-specific Lua scripts; their complete paths are in the raw inventory. Lua's reported search paths were:

```text
package.path=./?.lua;/usr/share/lua/?.lua;/usr/share/lua/?/init.lua;/usr/lib/lua/?.lua;/usr/lib/lua/?/init.lua
package.cpath=./?.so;/usr/lib/lua/?.so;/usr/lib/lua/loadall.so
```

`/usr/share/lua` was absent at inspection. Relative search paths depend on the process's working directory; avoid running privileged Lua code from an untrusted directory.

## Servers, daemons, and IPC

### Running at the snapshot

| Program | Role | Observed exposure/state |
|---|---|---|
| `uhttpd` | HTTP server, CGI, LuCI, ubus plugin | TCP 80 on IPv4 and IPv6 wildcard addresses; no HTTPS listener observed. `/www/cgi-bin/luci` is present. |
| `dnsmasq` | DNS forwarder and DHCP capability | TCP and UDP 53 on IPv4 and IPv6 wildcard addresses. LAN DHCP was disabled in configuration. |
| `dropbear` | SSH server | TCP 22 on `192.168.0.254`; old SSH algorithms. |
| `hostapd` / `wpad-mini` | Wi-Fi access point/authentication | Active on `wlan0`; not a general HTTP application server. |
| `odhcpd` | IPv6 DHCP/router-advertisement service | Process running; actual network function depends on configuration. |
| BusyBox `ntpd` | Time synchronization | Running as NTP client; configured NTP-server mode was disabled. |
| `ubusd` and `rpcd` | Local IPC bus and RPC service | Local services, not evidence that a public TCP port is open. |
| `procd`, `netifd`, `logd` | Service supervisor, network management, logging | Running; `procd` can supervise a custom long-running program. |

Installed but not observed running: BusyBox `telnetd`, BusyBox `crond`, and `wpa_supplicant`. Other supplied utilities include `pppd`, `etherwake`, `ubus`, `uci`, `wget`, `nc`, `iptables`, and `iw`. An installed executable or enabled init script does not establish that a service is running or reachable.

`ubus list` exposed: `dhcp`, `hostapd.wlan0`, `log`, `network`, `network.device`, `network.interface`, `network.interface.lan`, `network.interface.loopback`, `network.interface.wan`, `network.wireless`, `service`, `session`, `system`, and `uci`.

The firmware has no observed OpenSSL command/library, LuaSec module, or active uhttpd HTTPS listener. Do not assume a custom CGI or endpoint automatically inherits LuCI authentication. This firmware and its network services are obsolete; restrict development and access to a trusted network.

## Practical options

1. **Small compiled C service:** Cross-compile against this exact SDK and use the installed uClibc, `libubus`, `libuci`, or other libraries as needed. The SDK on the development machine supplies headers and linker files; the router supplies runtime libraries.
2. **Lua service:** Lua 5.1 plus `nixio` can provide sockets without installing a separate LuaSocket package. The `ubus` and `uci` modules can interact with router services and configuration; calls that modify configuration require separate care.
3. **HTTP CGI:** The existing `uhttpd` supports CGI and LuCI. A CGI is normally started per request, unlike a persistent daemon; adding an endpoint would require appropriate configuration, authentication design, and separate authorization.
4. **Shell scripts:** BusyBox ash and its applets are useful for small orchestration tasks. The raw inventory contains the complete 118 applet names. Avoid assuming GNU-specific command options.

No services were started, installed, reconfigured, or removed as part of this runtime inventory.
