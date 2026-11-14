# OpenWrt hello-world HTTP server

This is a small demonstration HTTP server for the inspected TP-Link TL-WR841N/ND v9 running OpenWrt 15.05.1 (`ar71xx/generic`). It listens on TCP port 8080 by default and returns `Hello World` for each request. Pass a different port as the first argument if needed.

The included `hello-http` binary was cross-compiled using the matching OpenWrt 15.05.1 uClibc SDK. It is approximately 5.7 KiB and dynamically links to the router's existing uClibc and libgcc. It is not a standalone static executable, and it is not intended to work on arbitrary OpenWrt versions or architectures.

## Test result

On 2026-11-14, the binary was copied to a unique directory under the router's RAM-backed `/tmp`, run on port 8080, and reached from the LAN:

```text
HTTP/1.1 200 OK
Content-Type: text/plain; charset=utf-8
Content-Length: 12
Connection: close

Hello World
```

The observed process `VmRSS` was 492 KiB and `VmSize` was 772 KiB. RSS can include shared library pages and is not the same as incremental system memory use; actual usage changes with activity. The test process and temporary files were removed afterwards. Nothing was installed on the router's flash or configured to start automatically.

## Build

Use the [OpenWrt 15.05.1 ar71xx/generic SDK](https://archive.openwrt.org/chaos_calmer/15.05.1/ar71xx/generic/OpenWrt-SDK-15.05.1-ar71xx-generic_gcc-4.8-linaro_uClibc-0.9.33.2.Linux-x86_64.tar.bz2). The archive's published SHA-256 is `9c8e92dd06988a9bda36c057b3a2bc5085d769fa3271064cb13b5b10044a9be3`; verify it before extracting. This SDK contains Linux x86-64 host tools. Once extracted, set `STAGING_DIR` to its `staging_dir` and put its cross-toolchain on `PATH`:

```sh
export STAGING_DIR=/path/to/extracted-sdk/staging_dir
export PATH="$STAGING_DIR/toolchain-mips_34kc_gcc-4.8-linaro_uClibc-0.9.33.2/bin:$PATH"
make
```

The Makefile targets MIPS32r2 with soft float and tunes for the router's 24Kc CPU. A generic Debian MIPS/glibc static build produced an illegal-instruction error on this router; use the matching SDK rather than assuming any MIPS binary will work.

## Run

Transfer `hello-http` to a unique path under `/tmp` using a compatible SSH/SCP client, then run it on the router:

```sh
chmod 700 /tmp/hello-http-demo
/tmp/hello-http-demo 8080
```

From a LAN client, visit `http://192.168.0.254:8080/` or run `curl http://192.168.0.254:8080/`. Stop the server with Ctrl+C and remove the temporary file when finished. `/tmp` is erased on reboot. The SSH client compatibility and observed host-key fingerprint are documented in `../router-inventory.md`; verify the fingerprint independently before trusting it.

**Security:** This is a minimal, single-threaded example, not a production server. It binds to all IPv4 interfaces and has no authentication or TLS. Do not expose it to an untrusted network.
