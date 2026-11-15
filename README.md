# OpenWrt router experiments

Software and notes for a TP-Link TL-WR841N v9 running OpenWrt 15.05.1 (`192.168.0.254`) on the home LAN. Its main job here is to log the data usage of the Huawei B310s-22 LTE router (`192.168.0.1`) that provides the internet connection.

## Layout

| Path | What it is |
|---|---|
| [`huawei-trafficd/`](huawei-trafficd/README.md) | Daemon that records the Huawei's download/upload/session counters, LTE signal quality and connection state, plus the `traffic.html` dashboard (usage, signal, connection log) and the `query.py` client |
| [`www/`](www/README.md) | Landing page for the router (`http://192.168.0.254/`), linking to LuCI and the dashboard |
| [`rtty/`](rtty/README.md) | rtty remote-terminal client built for the router (no TLS, 35 KiB on flash), kept for later; not installed |
| [`router-ssh/`](router-ssh/README.md) | `connect`, `copy` and `tunnel` helpers for the router's old Dropbear SSH server |
| [`docs/`](docs/) | Inventories and audits of the router, taken 2026-11-14 (see below) |
| `toolchain/` | The OpenWrt SDK, downloaded locally; not committed (see below) |

On the router:

- `http://192.168.0.254/`: landing page
- `http://192.168.0.254/traffic.html`: LTE usage, signal and connection dashboard
- `http://192.168.0.254/cgi-bin/luci`: LuCI admin (login required)

### `docs/`

| File | Contents |
|---|---|
| [`router-inventory.md`](docs/router-inventory.md) | Hardware, flash/RAM limits, network and services overview |
| [`router-runtime-inventory.md`](docs/router-runtime-inventory.md) | Libraries, interpreters and tools available for software on the router |
| [`router-runtime-raw.md`](docs/router-runtime-raw.md) | Raw listings behind the runtime inventory |
| [`ssh-inventory.md`](docs/ssh-inventory.md) | Detailed redacted SSH snapshot |
| [`installed-packages.md`](docs/installed-packages.md) | `opkg list-installed` |
| [`overlay-audit.md`](docs/overlay-audit.md) | What uses the writable flash, and the DDNS package removal |
| [`ubus-methods.md`](docs/ubus-methods.md) | ubus objects and methods, and how the `/ubus` HTTP bridge and its ACLs behave |

These describe the router as it was on 2026-11-14. Later changes are recorded in the component READMEs above. The documents contain network identifiers, so keep this repository private.

## The router's limits

- **CPU and C library:** big-endian MIPS32r2 (24Kc). OpenWrt 15.05.1 `ar71xx/generic` uses uClibc 0.9.33.2, not glibc or musl.
- **Storage and memory:** 4 MiB flash, of which only about 270–300 KiB is writable and free. 32 MiB RAM, of which a few MiB are free. `/tmp` is RAM.
- **What this means for software:** keep programs small and link them dynamically to libraries already on the router (`libubox`, `libubus`, `libblobmsg_json`, `libjson-c`, `libuci`, uClibc). Installing extra packages is rarely an option.

## Building for the router

Use the matching OpenWrt SDK. A generic Debian MIPS/glibc static build crashed on this router with an illegal instruction, so don't assume any MIPS binary will work.

1. **Download** the [OpenWrt 15.05.1 ar71xx/generic SDK](https://archive.openwrt.org/chaos_calmer/15.05.1/ar71xx/generic/OpenWrt-SDK-15.05.1-ar71xx-generic_gcc-4.8-linaro_uClibc-0.9.33.2.Linux-x86_64.tar.bz2) and verify its published SHA-256:

   ```text
   9c8e92dd06988a9bda36c057b3a2bc5085d769fa3271064cb13b5b10044a9be3
   ```

2. **Extract** it into `toolchain/`, which git ignores:

   ```sh
   mkdir -p toolchain && tar xjf OpenWrt-SDK-15.05.1-*.tar.bz2 -C toolchain
   ```

3. **Build:** for example `make -C huawei-trafficd`. The Makefile finds the SDK in `../toolchain/`; pass `SDK=/path/to/sdk` to use another location. It compiles for MIPS32r2 with soft float, tuned for the 24Kc, and links against the SDK's copies of the router's libraries.

**On an ARM64 build host** (such as an Arch Linux ARM VM): the SDK's tools are x86-64 Linux programs. They run under `qemu-user` binfmt emulation, given an x86-64 glibc runtime:

```sh
sudo pacman -S qemu-user qemu-user-binfmt
# Extract the x86_64 glibc, libgcc, libstdc++ and zlib packages into /opt/x86_64-root,
# with lib and lib64 symlinked to usr/lib, then:
QEMU_LD_PREFIX=/opt/x86_64-root make -C huawei-trafficd
```

Under emulation the SDK rebuilt an earlier example program byte for byte identical to its previously committed binary.

## Deploying

Copy files with `router-ssh/copy` and run commands with `router-ssh/connect`; see [`router-ssh/README.md`](router-ssh/README.md). Keep new files small, and test from `/tmp` (RAM) before writing to flash.
