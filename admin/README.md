# admin: router admin page without Lua

A single-page admin interface at **http://192.168.0.254/admin.html**, linked from the landing page as "Router admin". It runs entirely in the browser and talks to the router only through uhttpd's `/ubus` JSON-RPC bridge (rpcd), plus `cgi-io` for file uploads. No Lua runs on the router, so it can replace LuCI in a LuCI-free firmware image (step 2 of the plan: Image Builder). LuCI stays installed and linked as a fallback until then.

| | |
|---|---|
| Page | `/www/admin.html`, 52 KB (17 KB gzipped) |
| Helper | `/usr/libexec/admin-apply`, 3.8 KB shell script |
| ACL | `/usr/share/rpcd/acl.d/admin.json` |
| Packages added | `rpcd-mod-file`, `rpcd-mod-iwinfo`, `rpcd-mod-rpcsys`, `cgi-io` (official 15.05.1 packages, C, about 23 KB) |
| Flash used | 48 KB in total on install (overlay 324 → 372 KB used) |
| Installed | 2026-11-16 |

## What it does

| Tab | |
|---|---|
| **Status** | Model, firmware, kernel, router time, uptime, load, memory, flash; LAN/WAN up or down, address, gateway, DNS; Wi-Fi state, channel, power, country, security; the Wi-Fi clients with signal, link rates and idle time, named from [lanwatch](../lanwatch/README.md). Refreshes every 5 s. |
| **Wi-Fi** | On/off, network name, security (WPA2, WPA/WPA2, open), password, hidden; channel, width, transmit power and country. The channel, power and country lists come from the radio itself (`iwinfo`); country names come from the browser. A scan of nearby networks, with a count per channel. |
| **Network** | LAN address, netmask, gateway, DNS servers. Shows whether the DHCP server is on, and what the LAN is using right now (address, gateway, DNS, bridge ports, MAC). |
| **System** | Hostname; time zone (the POSIX TZ string the router needs is worked out from it, not shown); root password; services (start, stop, restart, enable or disable at boot); the system log with a filter; restart. |
| **Backup & firmware** | Download a settings backup; restore one; firmware upgrade (upload with progress, the router's own image check, size and MD5, keep settings or not); factory reset. |

Left out on purpose (still possible over SSH, or in LuCI while it's installed): firewall rules, the DHCP server's settings, static routes, IPv6, PPP and the package manager.

## How changes are applied

**Wi-Fi and network changes have a 60-second safety net.** After saving, the new settings are live, and the page asks **Keep changes** or **Undo now**. If nobody answers within 60 seconds, the router restores the previous settings by itself. So a change that cuts you off undoes itself. Some cases need a reconnect first:
- **Wi-Fi name or password changed:** devices on this router's Wi-Fi reconnect to the new network, then press Keep.
- **LAN address changed:** the dialog links to the new address. The login session goes with the link (in the `#` part of the URL, which isn't sent over the network), and the page there offers Keep. If the new address doesn't work, the old page notices when the old address comes back.

Hostname and time zone are applied without the safety net, since they can't cut you off.

This is done by `admin-apply`, not by rpcd. Two problems with rpcd 2015 on this router ruled it out (both found while testing):
- **rpcd's own `uci apply` with rollback never works:** it always answers "no data". Its check expects `glob()` to list `.` and `..`, which uClibc's doesn't.
- **Staged `uci set` changes leak between login sessions.** Changes staged in one session showed up in another's `uci changes`, and a commit could include them.

`admin-apply apply SECONDS OP ARG...` does the whole change in one call:
1. checks the arguments: only `set`, `delete` and `add_list`, and only existing config files;
2. snapshots the configs involved to `/tmp/admin-apply`;
3. makes the changes with the `uci` command, and commits them;
4. sends procd the same `config.change` event rpcd would, so the affected services reload;
5. starts the timer.

`confirm` keeps the change, `rollback` undoes it now, and `status` reports what's pending. A change that the timer undoes is logged as `admin: settings change not kept`.

Other workarounds in the helper, all found while testing:
- **Timer and background jobs are started with `start-stop-daemon -b`.** rpcd waits for a command's output pipe to close and kills it after 3 seconds. A plain `&` background job sometimes kept the pipe open.
- **Service start, stop and restart, and the reboot, also run detached (`admin-apply bg`).** A network restart can take longer than rpcd's 3-second limit, and rpcd would kill it halfway.
- **Backup and restore run through the helper (`admin-apply backup|restore`).** rpcd starts commands with stdin closed, and `sysupgrade --create-backup` then writes an empty archive. `cgi-io`'s own backup download (`cgi-backup`) is also broken in this version: it never ends its chunked reply, so Chromium rejects it. The page instead has the router create the backup, reads it as base64 with `file read`, and deletes it.

## Time zones without a table

LuCI ships a table mapping zone names to POSIX `TZ` strings. This page instead lists the zones the **browser** knows (`Intl`), and works out the POSIX string for the chosen zone from the browser's own time-zone data:
1. it finds this year's two clock changes, to the minute;
2. it writes them as `Mm.w.d/time` rules;
3. it names the zone with its abbreviation, or `<+03>` style when it has none.

Checked against the VM's zoneinfo database: **412 of 418 zones** give the same UTC offset all year, every 15 minutes.
- **4 Canadian zones differ** because the VM's tzdata is newer than the browser's.
- **2 Moroccan zones differ** because their DST follows Ramadan, which no POSIX rule can express.

The router's current `WET0WEST,M3.5.0/1,M10.5.0` (Lisbon) is reproduced exactly. uClibc on the router was checked with the unusual forms: `<+10>-10<+11>`, half-hour and 45-minute offsets, and southern-hemisphere DST.

## Security

- **Login:** rpcd's own login (`session login`) with the root password, the same check LuCI uses. Sessions expire after 30 minutes idle. When that happens, the page asks for the password again, then carries on with what you were doing.
- **What a session can do:** the ACL gives the logged-in root session every ubus object, every uci config and uploads. It's granted through the `'*'` read/write lists of the `root` login in `/etc/config/rpcd`. **Anonymous users get nothing new:** the anonymous ACLs of huawei-trafficd and lanwatch are unchanged.
- **Plain HTTP, as with LuCI here:** the password and session ID cross the LAN unencrypted (uhttpd has no TLS module installed).
- **Cross-site requests:** the session ID travels in the request body, not a cookie, so another website can't make requests in your name.
- **Text from the network:** everything that comes from the router or other devices (SSIDs from a scan, device names, log lines) is inserted as text, never as HTML.
- **Backups contain `/etc/shadow` and the Wi-Fi password.** Keep the downloaded file private.

## Install

```sh
# packages (from https://archive.openwrt.org/chaos_calmer/15.05.1/ar71xx/generic/packages/{base,packages}/)
./router-ssh/copy rpcd-mod-file_*.ipk rpcd-mod-iwinfo_*.ipk rpcd-mod-rpcsys_*.ipk cgi-io_1_ar71xx.ipk router:/tmp/
./router-ssh/connect 'cd /tmp && opkg install ./rpcd-mod-*.ipk ./cgi-io_*.ipk && rm *.ipk'

./router-ssh/copy admin/www/admin.html admin/files/admin-apply admin/files/admin.acl.json router:/tmp/
./router-ssh/connect 'cd /tmp && cp admin.html /www/ && cp admin-apply /usr/libexec/ && chmod 755 /usr/libexec/admin-apply \
  && cp admin.acl.json /usr/share/rpcd/acl.d/admin.json && rm admin.html admin-apply admin.acl.json && /etc/init.d/rpcd restart'
```

Restarting rpcd logs out open LuCI sessions.

**Uninstall:**

```sh
./router-ssh/connect 'rm /www/admin.html /usr/libexec/admin-apply /usr/share/rpcd/acl.d/admin.json; \
  opkg remove cgi-io rpcd-mod-rpcsys rpcd-mod-iwinfo rpcd-mod-file; /etc/init.d/rpcd restart'
```

## Verification performed (2026-11-16)

Run against the real router, in headless Chromium (desktop, and a phone in dark mode). There were no script errors, and nothing overflowed on the phone.

- **Login:** a wrong password is refused. An expired session (destroyed on the router mid-use) brings up the login, then the interrupted action finishes.
- **All five tabs load with real data.** The Wi-Fi clients are named from lanwatch.
- **`admin-apply` on the router, through ubus, on a throwaway config:**
  - **Values:** spaces and quotes in values are kept, and list changes work.
  - **Keep:** a kept change stays.
  - **Undo:** both a timed-out change and Undo restore the snapshot.
  - **One at a time:** a second change while one is pending is refused.
  - **Bad input** is rejected without touching anything: unknown option, unknown config, bad operation, missing argument, path tricks.
  - **Speed:** every call returns in 0.1–0.4 s, 21 calls in a row.
- **Save flows,** with the helper faked in the browser so nothing on the router changed:
  - Wi-Fi: only changed options are sent, using the real section names (for example `wireless.cfg033579.ssid`). Keep, Undo, and expiry all work.
  - Network: a gateway outside the subnet is refused. An address change asks first, then gives the hand-off link.
  - The new-address page picks up the session and the pending change.
  - Time zone: choosing America/New_York gives `EST5EDT,M3.2.0,M11.1.0`, saved without the safety net.
  - Password: a mismatched repeat is refused.
- **Real actions on the router:**
  - **Backup:** downloaded and complete. Its `etc/config/network` is identical to the router's.
  - **Firmware:** a random 300 KB "firmware" was uploaded, and the router's check rejected it ("Invalid image type"), so Flash stayed disabled. Cancel deleted the file.
  - **Restore:** a non-backup file was refused and deleted.
  - **Services:** restarting `tinymdns` from the page gave it a new process ID.

**Not run, because they would disrupt the LAN or the router:**
- saving real Wi-Fi or network changes;
- the Wi-Fi scan (it pauses this router's Wi-Fi briefly);
- changing the password (it's also the SSH password);
- restart, restore, flashing and factory reset.

The commands behind them are standard (`rpc-sys` from the 15.05.1 packages, `sysupgrade`, `/sbin/reboot`), and the parts around them were tested as above.
