# rtty for the router (no TLS), kept for later

A build of [rtty](https://github.com/zhaojh329/rtty) 9.2.1 for the TL-WR841N v9 (OpenWrt 15.05.1, MIPS32r2, uClibc 0.9.33). rtty connects out to an [rttys](https://github.com/zhaojh329/rttys) server, which serves a web terminal (and file transfer) into the router.

**Not installed.** Built on 2026-11-15 and only tested under emulation, not on the router.

| | |
|---|---|
| Binary | `rtty`, 74 KiB stripped, about **35 KiB** on flash after JFFS2 compression |
| Links to | only libraries already on the router: `libc.so.0` (uClibc), `libutil.so.0`, `libm.so.0`, `libgcc_s.so.1` |
| Built in | libev 4.33 (event loop) and inih r58 (config file parser), which the router doesn't have |
| RAM | about 60 KB of code and constants, plus a 75 KB buffer area that only uses memory as it's written |
| SHA-256 | `402561deef5bf333eeee6c0393a2f1e8d81628d2270d148f7bf776b5acd3b35f` |

## No TLS: what that means

This build has no encryption. Everything between the router and the `rttys` server crosses the network in plain text: the terminal session, any password typed at the router's login prompt, and transferred files. **Use it only with an `rttys` server on the LAN**, for example on the VM, never across the internet.

A TLS build was measured too. Linking in mbedTLS 3.6 with a minimal TLS 1.2 client configuration made it 214 KiB, about 107 KiB on flash. That doesn't fit alongside a full year of huawei-trafficd's flash history: 260 KiB was free, and the history grows to about 160 KiB. It would make sense built into the firmware instead (the Image Builder idea), where it would sit compressed in the read-only part.

## Build

From the workspace root, with the OpenWrt SDK in `toolchain/` (see the top-level README):

```sh
make -C rtty fetch   # download the pinned sources into rtty/src/ (git-ignored) and verify them
make -C rtty         # build rtty/rtty
```

On the ARM64 VM, prefix both commands with `QEMU_LD_PREFIX=/opt/x86_64-root`, as for huawei-trafficd.

**Pinned sources:**

| Source | Version | Check |
|---|---|---|
| rtty | commit `b0429e7` (v9.2.1 + 3 commits, 2026-11-07) | git commit |
| libev | 4.33 | SHA-256 |
| inih | r58 | SHA-256 |

rtty's `buffer` and `log` submodules come at the commits that rtty commit pins. Its `ssl` submodule isn't fetched.

**How it's built:** the downloaded code is **only compiled**. libev's `configure` and rtty's CMake files are never run; `ev-config.h` and `config.h` replace them:
- **`ev-config.h`:** libev uses epoll, eventfd, signalfd and inotify, which this kernel (3.18) and uClibc support. The Linux AIO and io_uring backends are off.
- **`config.h`:** rtty's version, without `SSL_SUPPORT`.
- **`O_PATH`:** uClibc 0.9.33's headers lack it, so it's defined as `010000000`, the value in this kernel's `asm-generic/fcntl.h`, which MIPS uses.

A rebuild produces the same code. Only embedded source-file paths in log messages can differ if the directory layout changes.

## Trying it later

**1. Run an `rttys` server on the LAN**, for example on the VM. It listens for devices on port 5912 and serves its web interface on 5913.

**2. Copy rtty to the router's `/tmp`** (RAM, so it writes nothing to flash and is gone after a reboot):

```sh
./router-ssh/copy rtty/rtty router:/tmp/rtty
./router-ssh/connect '/tmp/rtty -I wr841n -h <rttys-host> -a -v'
```

**3. Open the `rttys` web interface.** The router appears as `wr841n`. Opening a terminal shows the router's normal login prompt; rtty's `-f` and `--shell` options can skip it, but don't use them without TLS. `-t <token>` makes rtty authenticate to an `rttys` server configured with a token.

**4. To keep it running,** install it as a procd service (`/usr/sbin/rtty` plus an init script). That puts the 35 KiB on flash. Check `df -h /overlay` first, and remember huawei-trafficd's history needs room to grow.
