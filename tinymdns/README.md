# tinymdns: minimal mDNS / DNS-SD advertiser

Makes the router reachable as **`http://openwrt.local/`**, and advertises its web pages as discoverable web services (DNS-SD `_http._tcp`), without remembering its IP address. It works with recent Windows, macOS, iOS, Android and Linux (Avahi or systemd-resolved).

| | |
|---|---|
| Binary | 16.8 KiB stripped, about 12 KiB of flash used on install (binary, init script and config) |
| Links to | only libraries already on the router: `libubox`, `libgcc_s`, uClibc |
| RAM | about 540 KiB RSS, mostly shared library pages |
| Installed | 2026-11-15, enabled at boot (`/etc/init.d/tinymdns`) |

OpenWrt 15.05.1 has no small mDNS responder: Avahi's packages are about 138 KiB and Apple's `mdnsd` about 171 KiB.

## What it answers

- **`<hostname>.local`** (A record) with the router's IPv4 address. The hostname comes from the router's system settings (`OpenWrt` → `openwrt.local`) unless `hostname` is set.
- **The reverse name** of that address (`254.0.168.192.in-addr.arpa`) → `openwrt.local`.
- **The configured services:**
  - the service-type list (`_services._dns-sd._udp.local` → `_http._tcp.local`);
  - each instance (for example `LTE data usage._http._tcp.local`), with its SRV record (port and host) and TXT record (for example `path=/traffic.html`);
  - answers that include the related records, so a client gets everything in one reply.

## Protocol behaviour (RFC 6762 / RFC 6763)

- **Probing:** at startup, and whenever the address changes, it first asks three times, 250 ms apart, whether anyone else already uses its names. If another host does, it renames itself (`openwrt-2.local`, or `LTE data usage (2)`) and probes again. A host repeating our own identical records (an mDNS reflector, for example) isn't counted as a conflict.
- **Announcing:** it sends all its records unprompted 3 times (at 0, 1 and 2 s). On stop it sends a **goodbye** (TTL 0), so other devices drop the records straight away.
- **Answering:**
  - normal queries get a multicast answer, and the same record isn't multicast twice within a second;
  - queries asking for a direct reply (the QU bit) get one;
  - queries from ports other than 5353 (simple resolvers, `dig`) get a direct reply in plain-DNS form: question echoed, TTL at most 10 s, no cache-flush bit.
- **Interface:** it serves only packets that arrive on the configured device (`br-lan`), ignores its own packets, and uses TTL 255 as mDNS requires.
- **Changes:** every 10 s it checks the device's address and index. If the bridge is recreated, it rejoins the multicast group. If the address changes, it sends a goodbye for the old address, then probes and announces the new one.

**Deliberately left out**, because they don't matter for a single advertiser on a home LAN:
- IPv6 (AAAA records, and listening on IPv6);
- known-answer suppression;
- the random 20–120 ms delay before shared answers;
- tie-breaking when two hosts probe the same name at the same moment;
- name compression in packets it sends (they stay well under 1,460 bytes).

## Configuration

`/etc/config/tinymdns`:

```
config tinymdns 'main'
	option enabled '1'
	option interface 'lan'          # logical network; its device is looked up from netifd
	#option hostname 'openwrt'      # default: the system hostname, lowercased

config service
	option name 'LTE data usage'    # instance name: spaces allowed, no commas
	option type '_http._tcp'
	option port '80'
	list txt 'path=/traffic.html'   # key=value, no spaces

config service
	option name 'OpenWrt router'
	option type '_http._tcp'
	option port '80'
	list txt 'path=/'
```

There can be up to 4 services. Apply changes with `/etc/init.d/tinymdns restart`. The service also restarts automatically when the system hostname changes.

Command line: `tinymdns -i <device> -n <hostname> -s "<type>,<port>,<instance name>[,txt...]"`, with `-s` repeatable.

## Build and install

```sh
make -C tinymdns          # OpenWrt 15.05.1 SDK in ../toolchain (see the top-level README)

./router-ssh/copy tinymdns/tinymdns tinymdns/files/tinymdns.init tinymdns/files/tinymdns.config router:/tmp/
./router-ssh/connect
```

On the router:

```sh
cp /tmp/tinymdns /usr/sbin/ && chmod 755 /usr/sbin/tinymdns
cp /tmp/tinymdns.init /etc/init.d/tinymdns && chmod 755 /etc/init.d/tinymdns
cp /tmp/tinymdns.config /etc/config/tinymdns
/etc/init.d/tinymdns enable && /etc/init.d/tinymdns start
```

**Uninstall:**

```sh
/etc/init.d/tinymdns stop; /etc/init.d/tinymdns disable
rm /usr/sbin/tinymdns /etc/init.d/tinymdns /etc/config/tinymdns
```

## Security notes

- **Untrusted input:** every mDNS packet on the LAN reaches this parser. All reads are bounds-checked, compression pointers must point backwards (which rules out loops), names are limited to 255 bytes, and truncated packets are dropped. It was fuzzed with 8,000 malformed and random packets under AddressSanitizer and UBSan, with no findings.
- **Inherent to mDNS:** any host on the LAN can claim `openwrt.local`, or answer for it. This responder then renames itself rather than fighting, which is what the RFC requires. Don't rely on `.local` names for anything security-sensitive.
- **Scope:** it serves only `br-lan`; the WAN side never sees it.

## Verification performed (2026-11-15)

- **Test bed:** two isolated network namespaces on the VM, linked only to each other. The sanitizer-instrumented build (AddressSanitizer and UBSan) ran in one, and a Python mDNS client in the other.
- **All 23 checks passed:**
  - 3 probes, then 3 announcements, with correct TTLs and cache-flush bits;
  - A, reverse PTR, browsing, service-type listing and the extra related records;
  - case-insensitive names, and silence for unknown names;
  - the one-second rate limit;
  - QU direct replies;
  - simple resolvers via `dig` (A, SRV with TTL 10, reverse);
  - goodbye on stop;
  - renaming to `openwrt-2.local` after a simulated conflict;
  - 8,009 malformed packets with no crash and no sanitizer report.
- **Address change:** a goodbye for the old address, 3 probes, then announcements of the new one.
- **On the LAN after install:** it claimed `openwrt.local` (192.168.0.254) without a conflict.
  - **systemd-resolved** on the VM, an independent implementation, resolved `openwrt.local` and the "LTE data usage" service (`openwrt.local:80`, `path=/traffic.html`) over multicast.
  - **HTTP:** `http://openwrt.local/traffic.html` returned 200.
  - **`dig` via the multicast group** reported a timeout. A capture showed the router's correct direct reply: `dig` ignores it because it comes from 192.168.0.254, not 224.0.0.251.
