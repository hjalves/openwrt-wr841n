# Writable flash overlay audit

Read-only inspection on 2026-11-14, followed by removal of the two DDNS packages at the user's request. Sizes and runtime state can change.

## Removal outcome

After the inspection, `luci-app-ddns` and `ddns-scripts` were removed using `opkg`, in that order. The modified `/etc/config/ddns` was retained by the package manager. No Wake-on-LAN packages were removed.

Overlay available space increased from **252 KiB to 296 KiB** (44 KiB more); used space fell from 324 KiB to 280 KiB. `dnsmasq`, `etherwake` and `luci-app-wol` remained installed. An authenticated LuCI overview returned HTTP 200, and a UDP DNS query for `localhost` succeeded after removal.

## Follow-up inspection after DDNS removal

`df` still reported 280 KiB used and 296 KiB free. `du -ak /overlay` reported approximately 55 KiB of visible entries. The difference is not a measured amount of safely reclaimable space; JFFS2 accounts for flash differently from `du`, including filesystem overhead, compression, erase blocks and potentially obsolete data. No erase, reset or garbage-collection operation was attempted.

### Why `df` and `du` differ

The partition has nine 64 KiB erase blocks. In the upstream Linux 3.18.23 implementation, JFFS2 [calculates its normal-write reserve](https://github.com/gregkh/linux/blob/v3.18.23/fs/jffs2/build.c) as two blocks for deletions plus a rounded-up allowance for overhead. For this partition's geometry that formula gives three blocks, or **192 KiB**. The [upstream `statfs` implementation](https://github.com/gregkh/linux/blob/v3.18.23/fs/jffs2/fs.c) subtracts that reserve when reporting free space and counts `dirty_size` plus `free_size` before the reserve. Consequently, on that implementation `df`'s used figure includes the reserve; it is not simply the physical bytes occupied by live files.

This reserve is a source-based calculation, not a direct reading of the running OpenWrt kernel's internal counters; downstream patches could change the calculation. The `df` figure is also rounded, while `du` sums inode-reported blocks for visible files and does not measure their compressed on-flash representation. Remaining differences can include JFFS2 metadata and other internal accounting. No JFFS2-specific diagnostic counters were found under `/sys/fs/jffs2`, `/sys/kernel/debug/jffs2` or `/proc/fs/jffs2`, so the exact breakdown was not measured. Because upstream `statfs` already includes dirty space in its free estimate, garbage collection is not guaranteed to increase `df`'s available figure.

Comparing installed packages with the package database in `/rom` showed only `etherwake` and `luci-app-wol` absent from the firmware's package list. The remaining visible overlay also contains necessary configuration, SSH host keys, package metadata, and overlay whiteouts. Removing firmware-resident packages normally will not recover the underlying read-only SquashFS space.

Two tiny, potentially obsolete configuration files remain in writable storage: `/etc/config/ddns` (510 bytes), retained by `opkg` and possibly containing credentials, and `/etc/config/upnpd` (562 bytes). No installed UPnP package, running `miniupnpd` process, or executable at its standard path was observed. Their removal would discard saved settings and is unlikely to produce a meaningful increase in reported free space. Neither file was removed.

The previously tested hello-world executable is approximately 5.7 KiB and would fit within the currently reported free overlay space, though it was not installed there. A 500 KiB application is larger than the 296 KiB currently reported free, but the earlier conclusion that it could not fit was too categorical: JFFS2 compresses file data on flash, so a sufficiently compressible file might fit. Actual capacity depends on compression, metadata, filesystem behavior and operational headroom; it has not been tested with such an application.

### Compression tools observed

BusyBox supplies `gzip`, `gunzip`, `zcat`, `bunzip2`, `bzcat`, and `tar` with gzip support (`tar -z`). No `bzip2` compressor, `xz`, `lzma`, or `sha256sum` command was found. `md5sum` is available but is not suitable for adversarial integrity verification. The kernel's JFFS2 compression support does not imply corresponding user-space commands are installed.

A read-only gzip comparison of four Wake-on-LAN-related files yielded approximately 5.5 KiB compressed versus 11.3 KiB uncompressed. This is only a rough illustration; whole-file gzip sizes are not a measurement of JFFS2's actual on-flash occupancy or the flash that `opkg` would reclaim.

## Capacity and interpretation

- At the initial inspection, `/overlay` was a 576 KiB JFFS2 filesystem on `/dev/mtdblock3`; `df` showed **324 KiB used and 252 KiB available**. See the removal outcome above for the later measurement.
- The immutable SquashFS firmware is mounted at `/rom`; its 2.3 MiB is separate from the writable overlay. `/tmp` is RAM-backed, not flash-backed.
- `du -ak /overlay` displayed approximately 204 KiB for visible overlay entries. Do not subtract this from `df` or interpret the difference as a single hidden file: JFFS2's storage accounting, compression, metadata, erase blocks, and reclaimable/obsolete data differ from logical file sizes. The exact cause and amount of each contribution were not measured.
- Package file sizes below are **logical byte counts**, not guaranteed `df` savings. JFFS2 uses 64 KiB erase blocks, and actual available space may change differently or later. Package removal can preserve configuration files.

## Main removable candidates

At the initial inspection, the following package-owned regular files were found in `/overlay/upper` and did not have matching paths in `/rom`; this is stronger evidence that these particular files used the writable overlay, rather than merely being part of the immutable base image. The two DDNS packages have since been removed. Counts exclude some package-manager metadata and may include configuration files that `opkg` preserves.

| Package | Overlay file sizes | Purpose and considerations |
|---|---:|---|
| `luci-app-ddns` | 99,935 bytes (97.6 KiB) | LuCI Dynamic DNS interface. Depends on `ddns-scripts`; removing only this UI should leave the scripts package, but removes graphical management. |
| `ddns-scripts` | 38,293 bytes (37.4 KiB) | Dynamic DNS update scripts and configuration. `luci-app-ddns` depends on this package. Removal stops/removes Dynamic DNS functionality; this is **not** the router's normal dnsmasq DNS service. |
| `luci-app-wol` | 2,526 bytes (2.5 KiB) | LuCI Wake-on-LAN interface. Depends on `etherwake`. |
| `etherwake` | 9,873 bytes (9.6 KiB) | Wake-on-LAN tool, init script and configuration. Removing it eliminates that functionality. |

DDNS's two packages total approximately 135.0 KiB of listed file contents; Wake-on-LAN's two packages total approximately 12.1 KiB. These figures are not a prediction of physical flash reclaimed.

The configured DDNS entry `myddns_ipv4` was enabled but LuCI reported process ID 0, next update `stopped`, last update `never` at the time of inspection. This is a snapshot, **not proof that the service is no longer needed**. Confirm the intended use of DDNS and Wake-on-LAN before removing either.

Largest individual overlay file was `/usr/lib/lua/luci/model/cbi/ddns/detail.lua`, 45,740 bytes. The DDNS script `/usr/lib/ddns/dynamic_dns_functions.sh` was 23,950 bytes. The overlay's `/usr/lib/opkg/status` was 18,671 bytes and is package-manager state, not disposable cache.

## Other overlay data

- `/overlay/upper/etc` showed about 24 KiB in `du`, including network, wireless and firewall configuration, credentials and SSH host keys. These are operational state, not recommended cleanup targets.
- The overlay contains small backup account files and package metadata; their potential savings are small, and deleting them manually is not recommended.
- Character-device entries under `/overlay/upper/etc/uci-defaults` are overlay whiteouts, not ordinary files to clean up. Removing them can reveal files from `/rom` and change behavior.
- No sizeable flash-resident logs or download cache were identified. `/tmp/opkg-lists` is in RAM and was essentially empty at inspection.
- Deleting packages or files that exist only in `/rom` generally does not reclaim their read-only firmware space and can create overlay whiteouts.

## Removal commands and remaining option

First make a securely stored configuration backup and confirm you do not need the feature. Backups may contain passwords, Wi-Fi keys and other secrets. Removing packages can run maintenance scripts and write to flash. Check the resulting status and space after each step, and stop if anything unexpected occurs.

The following commands were run to remove both DDNS packages:

```sh
opkg remove luci-app-ddns
opkg remove ddns-scripts
df -h /overlay
```

If you do not need Wake-on-LAN, remove its dependent UI first, then its tool:

```sh
opkg remove luci-app-wol
opkg remove etherwake
df -h /overlay
```

Do not use forced removal or manually delete `/overlay/upper` files, package databases, whiteouts, host keys, or network configuration. Review whether anything else uses a feature before acting. No guaranteed amount of flash will be reclaimed without actually performing and measuring the removal.
