# Router inventory: 192.168.0.254

**Snapshot:** 2026-11-14, approximately 13:53–14:03 router local time. Dynamic values and client lists can change. This document contains network identifiers and should be treated as private. Passwords, Wi-Fi keys, cookies, and session tokens are intentionally omitted.

**Later change:** At the user's request, `luci-app-ddns` and `ddns-scripts` were subsequently removed. The modified DDNS configuration was retained, and overlay free space rose from 252 KiB to 296 KiB. DDNS package and process descriptions below are historical; see [overlay-audit.md](overlay-audit.md) for the removal and checks.

## Executive summary

- **Device:** TP-Link TL-WR841N/ND v9, running OpenWrt Chaos Calmer 15.05.1 with Linux 3.18.23.
- **Role:** The observed topology is consistent with a bridged wireless access point, rather than the primary internet gateway. LAN and Wi-Fi are bridged, LAN DHCP is disabled, and the default gateway is `192.168.0.1`.
- **Security:** The firmware and services are very old. LuCI is reachable over unencrypted HTTP, including the login; no HTTPS listener was found on port 443. SSH uses obsolete algorithms; a compatible older client authenticated successfully. Management and DNS are reachable from the LAN.
- **Hardware constraints:** 4 MiB flash and 32 MiB RAM. LuCI reported only 252 KiB free in writable storage. This severely limits safe firmware and package options; do not assume a current standard OpenWrt image fits.
- **Configuration worth reviewing:** WAN port forwards and DHCP reservations refer to `192.168.10.0/24`, whereas the active LAN is `192.168.0.0/24`. The WAN currently has no IP address. The DDNS entry is enabled but its process is stopped.

## Scope and method

Inspected only the specified router from `192.168.0.2`, via ICMP, a small TCP connect check, two UDP DNS queries, authenticated SSH, and authenticated LuCI GET/status requests. LuCI login created a temporary authenticated session, and the tests naturally generated logs and network connection state. No router configuration changes, router package installations, reboot, firmware operations, port-forward changes, active wireless scans, or tests against other hosts were performed. An ephemeral local Docker container installed an older SSH client and sshpass for the SSH checks; these were not installed on the router.

The package inventory is in [installed-packages.md](installed-packages.md); the detailed redacted SSH snapshot is in [ssh-inventory.md](ssh-inventory.md). The workstation lacked `nmap` and `sshpass`; Docker, OpenSSH, curl, Python sockets, and Python HTTP/HTML libraries were used. A full port scan, UDP scan, packet capture, vulnerability exploitation, serial console inspection, unredacted configuration export, and backup download were not performed. SSH host-key fingerprints were observed but not independently authenticated.

## Hardware and firmware

| Item | Finding | Source |
|---|---|---|
| Model | TP-Link TL-WR841N/ND v9; kernel boot argument `TL-WR841N-v9` | LuCI overview, kernel log |
| SoC / CPU | Qualcomm Atheros QCA9533 v1 rev 1, MIPS 24Kc, 550 MHz | Kernel log |
| Memory | 32 MiB physical; 29,265,920 bytes reported usable by LuCI | Kernel log, status |
| Flash | `mx25l3205d`, 4096 KiB | Kernel log |
| Flash layout | `u-boot` 0x000000–0x020000; `kernel` 0x020000–0x13f620; `rootfs` 0x13f620–0x3f0000; `rootfs_data` 0x360000–0x3f0000; `art` 0x3f0000–0x400000; `firmware` 0x020000–0x3f0000 | Kernel log; some regions overlap by design |
| Filesystems | SquashFS root, JFFS2 overlay | Kernel log |
| Writable free space | 252 KiB, 44% as displayed | LuCI software page |
| Firmware | OpenWrt Chaos Calmer 15.05.1 | LuCI |
| Kernel | 3.18.23, built 2016-01-31 | Kernel log |
| LuCI | 15.05-149-g0d8bbd2 Release, `git-15.363.78009-956be55` | LuCI footer |
| Switch | AR934X built-in switch, reported as `switch0` | LuCI, kernel log |
| Radio driver | ath9k; boot log identifies Atheros AR9531 Rev 1 | Kernel log, package list |

The historical OpenWrt [v9 hardware data](https://oldwiki.archive.openwrt.org/toh/hwdata/tp-link/tp-link_tl-wr841n_9) additionally describes five 100-Mbit/s Ethernet ports, 2.4 GHz 802.11b/g/n, no 5 GHz, no USB, and U-Boot. These are reference specifications, not independently verified physical observations. The kernel reports a 1000-Mbit/s `eth0` internal link; this does **not** establish that external ports are gigabit.

OpenWrt's [4/32 warning](https://openwrt.org/supported_devices/432_warning) explains the constraints of this class of device. Replacing it with supported hardware is generally safer than attempting an unverified flash or custom firmware. Verify the physical revision and a recovery plan before considering any firmware change.

## Network topology and interfaces

| Interface | Configuration / observed state |
|---|---|
| LAN `br-lan` | Static `192.168.0.254/24`; MAC `30:B5:C2:80:F4:BE`; up; firewall zone `lan` |
| Bridge members | Wired `eth0` and Wi-Fi `wlan0`; spanning tree unchecked |
| Default route | `0.0.0.0/0` via `192.168.0.1` over LAN |
| Connected route | `192.168.0.0/24` over LAN |
| LAN DNS settings | `1.1.1.1`, `1.0.0.1` |
| WAN `eth1` | Configured as DHCP client in `wan` zone; MAC `30:B5:C2:80:F4:BF`; status reported up but no IPv4/IPv6 address, no uptime, and zero traffic. This does not by itself prove cable/link state. |
| IPv6 | ULA prefix configured `fda2:32bc:5e0a::/48`; `ifconfig` showed link-local `fe80::32b5:c2ff:fe80:f4be/64` on `br-lan` and `wlan0`. No global or ULA interface address was observed. |
| Switch | VLAN functionality enabled; VLAN ID 1 lists CPU and ports 1–4 as untagged. Physical-port mapping was not independently verified. |

SSH's `swconfig` snapshot showed switch ports 2 and 4 up at 100 Mbit/s, ports 1 and 3 down, and internal port 0 up at 1000 Mbit/s. These are transient link states. Configuration also contains a WAN static route targeting `192.168.100.1`, which was not present in the active IPv4 route table.

The status overview labels the interface carrying the default route as its “WAN status,” but its JSON identifies that interface as static `br-lan`; it is not evidence of an active dedicated WAN connection.

### Neighbours observed in router ARP table

These are cached observations, not a complete host inventory. No other host was probed.

| IPv4 | MAC | Interface |
|---|---|---|
| `192.168.0.1` | `24:df:6a:13:ff:a5` | `br-lan` |
| `192.168.0.2` | `c8:60:00:1f:5f:f8` | `br-lan` |
| `192.168.0.201` | `90:ec:ea:e5:c4:b2` | `br-lan` |
| `192.168.0.204` | `54:60:09:49:8f:ee` | `br-lan` |

## Wireless

| Item | Finding |
|---|---|
| Radio / interface | `radio0`, `wlan0`; operational access-point mode, bridged to LAN |
| SSID | `wifi2`, not hidden |
| Standard / band | 2.4 GHz 802.11b/g/n |
| Channel | Configured `auto`; operating on channel 11, 2.462 GHz, 20 MHz width at the snapshot |
| Security | WPA2-PSK, CCMP observed; key intentionally not recorded |
| Transmit power | 19 dBm (approximately 79 mW) |
| Country code | `CN` (China). The boot log first showed US, then changed to CN; current LuCI status reports CN. Confirm that this matches the router's actual operating jurisdiction before any change. |
| Other settings | WMM enabled; MAC filtering disabled |
| BSSID | `30:B5:C2:80:F4:BE` |
| Snapshot noise | -95 dBm |

Associated stations at the snapshot, with signal reported by the router: `54:60:09:49:8f:ee` (-40 dBm), `90:ec:ea:e5:c4:b2` (-53 dBm), and `92:69:c8:d9:cf:32` (-48 dBm). Associations and signal levels are transient; locally administered/randomized MAC addresses should not be treated as stable identities.

## DHCP and DNS

- LAN DHCP server is explicitly ignored/disabled. The dormant LAN pool fields show start 100, limit 150 and 12-hour leases; those fields are not proof of an active pool.
- dnsmasq is running; TCP and UDP port 53 responded. A UDP `localhost` A query succeeded. A `version.bind` CHAOS TXT query disclosed `dnsmasq-2.73`.
- Configured forwarders: `1.1.1.1`, `1.0.0.1`; `resolvfile` is `/tmp/resolv.conf.auto`. Local domain is `lan`; domain-needed, DNS rebinding protection, localise queries and expand hosts are enabled. TFTP is disabled.
- LuCI reported no active DHCP or DHCPv6 leases at the snapshot. No extra hostname entries were configured.

Configured static DHCP reservations, although LAN DHCP is disabled and these IPs are outside the active subnet:

| Name | MAC | Reserved IP |
|---|---|---|
| atom | `f0:79:59:25:d0:71` | `192.168.10.254` |
| thinkpad | `58:00:e3:ca:ad:b9` | `192.168.10.250` |
| atom-wlan | `40:e2:30:5a:5b:15` | `192.168.10.253` |
| desktop | `54:04:a6:3c:a2:99` | `192.168.10.10` |
| rpi | `b8:27:eb:6a:6c:9f` | `192.168.10.20` |

## Firewall and forwarding

- Global configured defaults: input ACCEPT, output ACCEPT, forward REJECT; SYN-flood protection enabled. The live IPv4 FORWARD chain's base policy was DROP with delegated rules, so the effective behavior should be interpreted from the complete live chains rather than equating the configuration label with the chain policy.
- `lan` zone: input/output/forward ACCEPT; `lan` → `wan` forwarding configured. `wan` zone: input REJECT, output ACCEPT, forward REJECT, masquerading and MSS clamping enabled.
- Enabled WAN TCP port forwards: external port 22 → `192.168.10.254:22`; external port 80 → `192.168.10.254:80`. The live NAT page showed zero packets for these rules at the snapshot. Their presence does not establish reachability from the internet: the WAN had no address, and upstream/network topology was not tested. They may become relevant if the WAN configuration or topology changes.
- Enabled WAN exceptions include DHCP renewal, IPv4 ping and IGMP, DHCPv6, MLD and ICMPv6, and ESP/UDP 500 forwarding to LAN. No user-added custom firewall commands were displayed.
- Bridged traffic and access from LAN should not be assumed protected by the WAN-zone rules. Live IPv4 LAN input was accepted.

## Management and exposure

### TCP checks from the LAN

Open: **22, 53, 80**. Port 80 served LuCI over HTTP. Port 443 did not accept a TCP connection. The full tested set was `21, 22, 23, 25, 53, 67, 80, 81, 110, 111, 123, 139, 143, 161, 443, 445, 465, 514, 515, 587, 631, 853, 993, 995, 1900, 2049, 2222, 3000, 3306, 3389, 5000, 5353, 5900, 8000, 8080, 8081, 8443, 9000, 9090, 10000`. Results apply only to the tested source, TCP ports, and moment; an unresponsive port is not proof that a service is absent on every interface or protocol.

HTTP exposes the administrator login and session to observers on the network path. LuCI authenticated successfully using the supplied root account; no password is stored in this document. The `uhttpd` process listened on `0.0.0.0:80` and `[::]:80`; actual reachability remains subject to interface addressing and firewall rules. UCI contains HTTPS listen and redirect settings, but the actual process had no port 443 listener. The reason was not established.

### SSH

- Banner: `dropbear_2015.67`; LuCI config limits Dropbear to LAN port 22. Password authentication and root password login are enabled; gateway ports are disabled.
- Server advertised `diffie-hellman-group14-sha1` and `diffie-hellman-group1-sha1` key exchange, `ssh-rsa` and `ssh-dss` host-key algorithms, and SHA-1/MD5 MACs. The workstation's OpenSSH 10.6 failed before authentication even with per-connection legacy options; router logs reported `No matching algo hostkey` for those attempts. The exact incompatibility has not been established.
- Debian OpenSSH 9.2 in an ephemeral Docker container successfully authenticated as root with the supplied password using `diffie-hellman-group14-sha1`, `ssh-rsa`, `aes128-ctr` and `hmac-sha1`. DSA also completed key exchange but is **not required**. RSA and DSA private host-key files both exist; no private-key contents were copied.
- Observed RSA 2048-bit fingerprint: `SHA256:jjM7WpR5nlKuqqVZKJaW3YSbk3cZWBOHqVBxKPrSmis`. Observed DSA 1024-bit fingerprint: `SHA256:8H8LNcftNm2i0zeDd3dr9CjqYa4S/gp382NKdcvitb8`. These fingerprints were obtained over the same unverified network connection and must be checked independently before trusting them. No host keys were regenerated and Dropbear was not restarted.
- An authenticated read-only shell inventory is in [ssh-inventory.md](ssh-inventory.md). The router lacks standalone `ip` and `iwinfo` commands; alternative available utilities were used.

To connect interactively from the same Docker host, after independently checking the displayed host-key fingerprint:

```sh
docker run --rm -it --network host i386/debian:bookworm-slim sh -c \
  'apt-get update -qq && apt-get install -y -qq openssh-client && \
   ssh -o KexAlgorithms=diffie-hellman-group14-sha1 -o HostKeyAlgorithms=ssh-rsa root@192.168.0.254'
```

Enter the password at the SSH prompt; do not place it in the command line. The container is temporary, so its known-hosts file is not retained. These legacy algorithms are weak and should be used only on a trusted management network.

## Services, startup and logs

- Observed running user-space processes included procd, ubusd, logd, rpcd, netifd, odhcpd, Dropbear, uhttpd, dnsmasq, ntpd and hostapd. This is a momentary process snapshot.
- SSH's init-script checks reported boot, cron, ddns, dnsmasq, done, Dropbear, etherwake, firewall, led, log, network, odhcpd, rpcd, sysctl, sysfixtime, sysntpd, system, telnet and uhttpd enabled; `umount` was disabled. Enabled does not mean currently running: TCP port 23 did not respond and there was no telnet listener in `netstat`.
- `/etc/rc.local` contained only default comments and `exit 0`. The LuCI scheduled-tasks page showed no tasks.
- NTP client enabled, server disabled; configured servers `0.openwrt.pool.ntp.org` through `3.openwrt.pool.ntp.org`. Timezone configured as `Europe/Lisbon`.
- DDNS configuration `myddns_ipv4` is enabled for `galisteu.nsupdate.info`; LuCI reported registered IP `87.196.80.94`, last update `never`, next update `stopped`, and process ID 0. This is a stored/reported value, not proof of the current public IP or external accessibility.
- System log mostly showed normal periodic WPA group-key handshakes and station association/inactivity events, plus the SSH negotiation failures caused by this inspection. The kernel log recorded normal boot, a deprecated automatic conntrack helper warning, and a low-entropy message early in boot followed by initialization. No conclusion about historical security incidents can be drawn from the limited in-memory log.

## Runtime snapshot

- Router local time: `Sat Nov 14 13:55:33 2026`.
- Uptime: 1,875,143 seconds, approximately 21 days 16 hours 52 minutes.
- Memory: 29,265,920 bytes total available, 6,529,024 bytes free, 2,199,552 bytes buffered; no swap reported.
- Connection tracking: 75 entries of a reported maximum 16,384. Values fluctuate.
- ICMP: 2/2 replies, approximately 3.8–3.9 ms round trip from the inspecting workstation.
- LuCI software page reported no available package lists. The separate inventory now uses full versions from `opkg list-installed` over SSH.

## Suggested next steps — not performed

1. Treat the supplied administrator password as exposed: it was shared in chat and sent over HTTP. From a trusted, isolated management network, plan a credential rotation and review who can reach the device. Avoid using this old HTTP interface on untrusted networks.
2. Prefer replacing this end-of-life 4/32 device with supported hardware. If retaining it, isolate it and document the residual risk. Do not install firmware or packages without checking exact hardware compatibility, space, backups and recovery procedures.
3. Review whether the enabled WAN port forwards, old `192.168.10.x` reservations, and stopped DDNS configuration are still needed. They may be intentional; do not remove them without confirming their purpose.
4. Use the compatible SSH client only from a trusted network, and verify the RSA fingerprint through an independent trusted channel. Avoid weakening system-wide SSH settings; the precise OpenSSH 10.6 incompatibility remains unconfirmed.
5. Verify the Wi-Fi regulatory country against the actual installation location, and ensure the SSID and security settings are intentional.
