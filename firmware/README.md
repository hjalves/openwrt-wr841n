# firmware: LuCI-free image for the TL-WR841N/ND v9

A custom OpenWrt 15.05.1 image for this router, built with the official **Image Builder** (official packages, same kernel), without LuCI and the parts an access point doesn't use. Everything we added is built in, rather than sitting in the small writable part of the flash.

**Status:** built and inspected on the VM (2026-11-16). **Not flashed.** Flashing is step 3, only with an explicit go-ahead.

## What changes

| | Official 15.05.1 (installed now) | This image |
|---|---|---|
| Image file (sysupgrade) | 3,407,876 bytes | 2,883,588 bytes |
| Kernel | | byte-identical to the official one |
| Root filesystem (read-only) | 2,196,018 bytes, 1101 entries | 1,660,674 bytes, 721 entries |
| **Writable flash (overlay)** | **576 KiB**, of which 204 KiB free today | **1088 KiB**, nearly all free |

**Removed** (30 packages):
- **LuCI and Lua:** `luci`, `luci-base`, `luci-mod-admin-full`, `luci-app-firewall`, `luci-theme-bootstrap`, `luci-lib-ip`, `luci-lib-nixio`, `luci-proto-ipv6`, `luci-proto-ppp`, `lua`, `liblua`, `libiwinfo-lua`, `libubus-lua`, `libuci-lua`.
- **IPv6:** `kmod-ipv6`, `ip6tables`, `kmod-ip6tables`, `kmod-nf-ipt6`, `kmod-nf-conntrack6`, `odhcpd`, `odhcp6c`.
- **PPP:** `ppp`, `ppp-mod-pppoe`, `kmod-ppp`, `kmod-pppoe`, `kmod-pppox`, `kmod-slhc`, `kmod-lib-crc-ccitt`.
- **`dnsmasq`:** this router's DHCP server is off (the Huawei does DHCP). Without dnsmasq, the router sends its own DNS lookups straight to the LAN's DNS servers (1.1.1.1, 1.0.0.1): at boot, `/tmp/resolv.conf` points to the list netifd writes.

**Added:**
- **Packages:** `rpcd-mod-file`, `rpcd-mod-iwinfo`, `rpcd-mod-rpcsys` and `cgi-io`, for the [admin page](../admin/README.md).
- **Our programs**, with their configs, ACLs and pages:
  - [huawei-trafficd](../huawei-trafficd/README.md), [lanwatch](../lanwatch/README.md) and [tinymdns](../tinymdns/README.md), enabled at boot through `/etc/rc.d` links in the image;
  - the [landing page](../www/README.md), `routerinfo`, the icons;
  - the admin page and `admin-apply`.
- **`/etc/uci-defaults/99-router-experiments`:** runs once at first boot. There's no IPv6 any more, so it:
  - sets the firewall's `disable_ipv6`;
  - removes uhttpd's `[::]` listeners.

  It works whether the settings were kept from the old firmware or are fresh defaults.
- **`/lib/upgrade/keep.d/router-experiments`:** future firmware upgrades with "keep settings" also keep `/srv/huawei-trafficd/` and `/srv/lanwatch/` (traffic history, device list).

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

**Not checked, because it needs the real hardware:** booting the image. In particular:
- whether the firewall (fw3) starts cleanly without IPv6 (it is told not to use it);
- whether DNS works without dnsmasq.

Both are standard OpenWrt configurations.

## Before flashing (step 3)

- **Keep the history:** add `/srv/huawei-trafficd/` and `/srv/lanwatch/` to the router's current `/etc/sysupgrade.conf`. The old firmware decides what a flash keeps, and it doesn't know about `/srv`. Otherwise the traffic history and device list start over.
- **Back up the settings** (admin page, Backup & firmware).
- **Check this hardware revision's recovery options**, in case the router doesn't boot. A serial console may be the only fallback.
- **Flash with settings kept,** from the admin page or with `sysupgrade` over SSH. Settings, passwords and SSH host keys carry over.

  Programs installed into the writable area go away, but they're all in the image now. Old config files whose packages are gone (`luci`, `ucitrack`, `dhcp`) stay, unused.
