# firmware: LuCI-free image for the TL-WR841N/ND v9

A custom OpenWrt 15.05.1 image for this router, built with the official **Image Builder** (official packages, same kernel), without LuCI and the parts an access point doesn't use. Everything we added is built in, rather than sitting in the small writable part of the flash.

**Status:** **flashed on 2026-11-16 at 22:54**, with settings kept, and checked afterwards (see "Flash and result" at the end). The image is `sha256 0b7a3787…8ca2c00`.

## What changes

| | Official 15.05.1 (installed now) | This image |
|---|---|---|
| Image file (sysupgrade) | 3,407,876 bytes | 2,883,588 bytes |
| Kernel | | byte-identical to the official one |
| Root filesystem (read-only) | 2,196,018 bytes, 1101 entries | 1,660,662 bytes, 721 entries |
| **Writable flash (overlay)** | **576 KiB**, of which 204 KiB free today | **1088 KiB**, nearly all free |

**Removed** (30 packages):
- **LuCI and Lua:** `luci`, `luci-base`, `luci-mod-admin-full`, `luci-app-firewall`, `luci-theme-bootstrap`, `luci-lib-ip`, `luci-lib-nixio`, `luci-proto-ipv6`, `luci-proto-ppp`, `lua`, `liblua`, `libiwinfo-lua`, `libubus-lua`, `libuci-lua`.
- **IPv6:** `kmod-ipv6`, `ip6tables`, `kmod-ip6tables`, `kmod-nf-ipt6`, `kmod-nf-conntrack6`, `odhcpd`, `odhcp6c`.
- **PPP:** `ppp`, `ppp-mod-pppoe`, `kmod-ppp`, `kmod-pppoe`, `kmod-pppox`, `kmod-slhc`, `kmod-lib-crc-ccitt`.
- **`dnsmasq`:** this router's DHCP server is off (the Huawei does DHCP). Without dnsmasq, the router sends its own DNS lookups straight to the LAN's DNS servers (1.1.1.1, 1.0.0.1): at boot, `/tmp/resolv.conf` points to the list netifd writes.
- **Firewall, from the next build on (2026-11-24):** `firewall`, `iptables`, `libxtables`, `libip4tc`, `libip6tc`, `kmod-nf-conntrack`, `kmod-nf-nat`, `kmod-nf-nathelper`, `kmod-ipt-core`, `kmod-nf-ipt`, `kmod-ipt-conntrack`, `kmod-ipt-nat`.
  - **Why:** since the WAN port joined the LAN (2026-11-19), the only zone, `lan`, accepted everything, with no rules, while the firewall still loaded about 30 kernel modules and tracked the router's own connections. The Huawei does the routing, NAT and filtering. Bridged traffic between LAN devices was never filtered here.
  - **Result:** the image shrinks by one 64 KiB flash block (2,883,588 → 2,818,052 bytes), so the writable area grows from 1088 to 1152 KiB. It also saves the modules' RAM.
  - **Related changes:** the image carries its own `/etc/sysctl.conf`, OpenWrt's without the netfilter, bridge-netfilter and IPv6 keys, which would otherwise be reported as unknown at every boot (the IPv6 ones already were). The first-boot script only touches the firewall config where one exists. After a "keep settings" upgrade from the current firmware, the old `/etc/config/firewall` stays behind unused; it can be deleted.
  - **Left as is:** `lib/netifd/dhcp.script` still calls `fw3`, but only for IPv6-in-IPv4 (6rd) tunnels, which this router never sets up. The admin page lists `firewall` among the services it warns about stopping; with no such service it just never appears.
- **`uboot-envtools`, from the next build on:** this TP-Link bootloader keeps no usable environment, and nothing used it.

**Added:**
- **Packages:** `rpcd-mod-file`, `rpcd-mod-iwinfo`, `rpcd-mod-rpcsys` and `cgi-io`, for the [admin page](../admin/README.md).
- **Our programs**, with their configs, ACLs and pages:
  - [huawei-trafficd](../huawei-trafficd/README.md), [lanwatch](../lanwatch/README.md) and [tinymdns](../tinymdns/README.md), enabled at boot through `/etc/rc.d` links in the image;
  - [weatherd](../weatherd/README.md) and its page, from the next build on (added 2026-11-17; the router currently runs it from the writable area);
  - [tvremote](../tvremote/README.md) and its page, from the next build on (added 2026-11-19; the router currently runs it from the writable area);
  - the [landing page](../www/README.md), `routerinfo`, the icons;
  - the admin page and `admin-apply`.
- **`/etc/uci-defaults/99-openwrt-wr841n`:** runs once at first boot. There's no IPv6 any more, so it:
  - sets the firewall's `disable_ipv6`;
  - removes uhttpd's `[::]` listeners.

  Since 2026-11-24 it skips the firewall settings where there's no firewall config (images without the firewall).

  Since 2026-11-19 it also makes the **WAN port a fifth LAN port**, as the router has been set up by hand since that day: the powerline to the Huawei is plugged into it. If the config still has a `wan` interface, the script:
  - adds its port (`eth1`) to the `lan` bridge;
  - deletes `wan`, `wan6` and routes over them;
  - deletes everything in the firewall about `wan`: the zone, forwarding, rules and port forwards.

  Without this, a factory reset or a flash without kept settings would cut the powerline (and so the Huawei and the internet) off from the router's LAN side. Where there's no `wan` interface it changes nothing.

  It also **restores the router's address** after a factory reset. If `lan` still has OpenWrt's default `192.168.1.1`, it sets `192.168.0.254/24`, gateway `192.168.0.1` (the Huawei) and DNS `1.1.1.1 1.0.0.1`. That default address is unreachable here, and this image has no DHCP server. Any other address, such as one kept from earlier settings, is left alone. Not restored: the Wi-Fi (OpenWrt's default is off) and the root password, which are secrets and don't belong in the image.

  It works whether the settings were kept from the old firmware or are fresh defaults.
- **`/lib/upgrade/keep.d/openwrt-wr841n`:** future firmware upgrades with "keep settings" also keep `/srv/huawei-trafficd/`, `/srv/lanwatch/` and `/srv/weatherd/` (traffic history, device list, weather history).

Because our programs live in the read-only part, a factory reset no longer removes them. The admin page's texts were updated to say so. The landing page's LuCI link now only appears where LuCI exists.

## Build

```sh
# Image Builder (verify against the published sha256sums), into toolchain/ which git ignores:
curl -LO https://archive.openwrt.org/chaos_calmer/15.05.1/ar71xx/generic/OpenWrt-ImageBuilder-15.05.1-ar71xx-generic.Linux-x86_64.tar.bz2
tar xjf OpenWrt-ImageBuilder-15.05.1-*.tar.bz2 -C toolchain

firmware/native-squashfs.sh      # only on non-x86 hosts (see below)
firmware/build.sh                # images in firmware/bin/ (git-ignored)
```

`build.sh` puts the files into `firmware/.stage/`, points the Image Builder at the archive.openwrt.org package indexes (the old `downloads.openwrt.org` URLs no longer serve 15.05.1), and builds the `TLWR841` profile. The image for this router is `openwrt-15.05.1-ar71xx-generic-tl-wr841n-v9-squashfs-sysupgrade.bin`.

The daemons are taken from the committed binaries. They are byte-identical to the ones running on the router (checked by MD5).

**On an ARM64 host:** the Image Builder's tools are x86-64 programs with their own bundled libraries, and run under `qemu-user` binfmt as they are. The exception is `mksquashfs4`, which crashes under emulation every time. `native-squashfs.sh` builds the same tool natively: OpenWrt 15.05's squashfs-tools 4.2, MD5-checked, with OpenWrt's 8 patches from the 15.05 tree. It then swaps it into the Image Builder, keeping the original as `mksquashfs4.x86`.

## Checks performed (2026-11-16)

- **Layout:** the official image, measured the same way, gives the router's current 576 KiB writable area exactly (its `rootfs_data` partition is `0x90000`), so the method is right. The new image gives 1088 KiB.
- **Kernel and TP-Link header:** identical to the official image, including hardware ID `0x08410009` and revision `r48532`.
- **File system format:** squashfs 4.0, xz, 256 KiB blocks, with exactly the same xz dictionary and filter options as the official image. The Image Builder's original x86 `unsquashfs` unpacks it into the same files as the native one.
- **Contents:** compared with the official image, package by package and file by file. The only differences are the ones listed above. No `lua`, LuCI, `dnsmasq`, `pppd`, or IPv6/PPP kernel modules remain.
- **First-boot script:** run on the router itself, with its own `uci`, on copies of the configs in `/tmp`. It was checked both with the router's current configs (as a "keep settings" upgrade leaves them) and with the image's defaults (as after a factory reset). Both ended with no `[::]` listeners and `disable_ipv6 '1'`, and the real configs were untouched.

- **First-boot script, WAN-port part (2026-11-19):** run the same way on copies of three configs:
  - the router's current ones (already bridged by hand): nothing changed;
  - the ones from before the manual change: `lan` became `eth0 eth1`, with no `wan` left in the network or firewall config;
  - the image's factory firewall with that network config: the same result.

  A second run changed nothing further, and the real configs were untouched. With the address step added, a factory-like config (`192.168.1.1`, no gateway or DNS) came out as `192.168.0.254` with gateway and DNS, and a kept custom address (`192.168.0.250`) was left alone.

- **Without the firewall (2026-11-24):**
  - **Package list:** the built image's list differs from the running router's by exactly the 13 removed packages, with none added.
  - **Leftover references:** none in the image, apart from the `dhcp.script` 6rd call and the admin page's service list noted above.
  - **First-boot script:** with no firewall config and a factory-like network, it ran without errors and gave `eth0 eth1` at `192.168.0.254`. The current config was again left unchanged.

**Not checked, because it needs the real hardware:** booting the image. In particular:
- whether the firewall (fw3) starts cleanly without IPv6 (it is told not to use it);
- whether DNS works without dnsmasq.

Both are standard OpenWrt configurations.

## Before flashing (step 3)

**Done 2026-11-16:**
- **`/srv` added to the router's `/etc/sysupgrade.conf`.** The old firmware decides what a flash keeps, and didn't know about `/srv`. `sysupgrade -l` now lists 40 files to keep: the settings, passwords, SSH host keys, `/srv/huawei-trafficd/2026-11.log` and `/srv/lanwatch/devices.dat`.
- **Backups in `firmware/backup/2026-11-16/`.** This folder is git-ignored and readable only by the owner, because it contains the password hash and Wi-Fi key.
  - `settings.tar.gz`: the settings backup, `sysupgrade -b`.
  - `overlay.tar.gz`: the whole writable area.
  - `mtd0.bin`…`mtd5.bin`: raw copies of every flash partition, including the bootloader and `art` (this unit's Wi-Fi calibration, which can't be downloaded anywhere). The bootloader and `art` copies match the router's own MD5s.
  - The router runs exactly the official 15.05.1 image: its root file system is byte-identical, and so is its kernel, apart from the TP-Link header's MD5 field.
- **Recovery images** in `firmware/backup/recovery/`, already named `wr841nv9_tp_recovery.bin`:
  - `official-15.05.1/`: the official factory image, matching OpenWrt's published SHA-256;
  - `openwrt-wr841n/`: the factory image from this build.

  Both carry the hardware ID the bootloader checks, and contain no bootloader of their own.

**Recovery options, if the new firmware misbehaves:**
1. **Settings-level problem, router reachable:** fix it over SSH, or flash back with "keep settings".
2. **Router boots but is unreachable:** OpenWrt failsafe mode. Press the reset button while the LED blinks during boot; the router then answers at 192.168.1.1 with no password.
3. **Router doesn't boot: TFTP recovery in the bootloader.** This router's U-Boot (1.1.4, built 2014-10-13) has TP-Link's recovery code: the decompressed bootloader contains `setenv ipaddr 192.168.0.86`, `setenv serverip 192.168.0.66`, `_tp_recovery.bin` and "product id verify".
   - Connect a **wired** computer to a LAN port, with the router unplugged from the rest of the network.
   - Set the computer to 192.168.0.66/24 and run a TFTP server serving one of the recovery images.
   - Hold the reset button while powering on, until the rightmost LED lights.
   - The router fetches and writes the image, which takes a few minutes. This follows TP-Link's own guide, which uses this model (`wr841nv9_tp_recovery.bin`) as its example.
   - This VM can't serve it: its network is bridged over Wi-Fi.
4. **Last resort:** the serial console on the board.

## Flash and result (2026-11-16)

**How it was flashed:**
- The image was copied to `/tmp/firmware.bin` and its MD5 checked on the router.
- `sysupgrade -T` passed.
- It was then started detached from the SSH session, so the disconnect couldn't interrupt it: `start-stop-daemon -S -b -p /var/run/sysupgrade.nopid -x /bin/sh -- -c "sysupgrade -v /tmp/firmware.bin >/tmp/sysupgrade.log 2>&1"`.
- sysupgrade saved the 40 kept files, `/srv` included.
- **The router was offline for about 35 seconds:** it stopped answering ping 31 s after the start and answered again at 65 s.

**Checked afterwards:**
- **Firmware:** 59 packages; no Lua, LuCI, dnsmasq, or loaded IPv6 modules.
- **First-boot script:** ran once and deleted itself. `firewall.@defaults[0].disable_ipv6=1`, and uhttpd listens on `0.0.0.0:80` and `:443` only.
- **Writable flash:** 1088 KB, **856 KB free** (was 576 KB, 204 KB free). Free RAM is about 15 MB (was about 11.5 MB). Our kept files take 38 KB; the rest of the "used" figure is JFFS2's reserved blocks.
- **Services running:** huawei-trafficd, lanwatch, tinymdns, uhttpd, rpcd, dropbear, netifd, sysntpd and logd.
- **Firewall** started with no errors (78 iptables rules).
- **DNS without dnsmasq:** `/tmp/resolv.conf` lists 1.1.1.1 and 1.0.0.1, and lookups work. NTP set the clock.
- **Kept data:**
  - `/srv/huawei-trafficd/2026-11.log`, `/srv/lanwatch/devices.dat`, the settings, the password and the SSH host keys all came through;
  - huawei-trafficd records again (signal, latency);
  - lanwatch knows 15 devices;
  - `openwrt.local` resolves.
- **Wi-Fi:** the `wifi2` network is up and its clients reconnected. Its config is identical to the pre-flash backup.
- **Web pages,** in headless Chromium, with no errors: the landing page (LuCI link hidden), `traffic.html`, `devices.html`, and every tab of the admin page (DHCP shown as "Not installed").

Old config files whose packages are gone (`luci`, `ucitrack`, `dhcp`, `upnpd`) came along with the kept settings. They're unused and harmless.

**Note on SSH:** this Dropbear refuses command lines over 1024 characters ("exec request failed"). Send longer scripts on standard input instead: `./router-ssh/connect 'sh -s' < script.sh`.
