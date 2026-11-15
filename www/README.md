# Router landing page

`index.html` replaces the router's default `/www/index.html`, which only redirects to LuCI. It is served at **http://192.168.0.254/** and links to:

- **Router admin:** LuCI at `/cgi-bin/luci`. This LuCI version serves its login form with HTTP status 403; that is normal. The card also shows the router's **uptime, memory available and flash free**, from `cgi-bin/routerinfo` (below).
- **Devices on the network:** the [`lanwatch`](../lanwatch/README.md) page at `/devices.html`. The card shows how many devices are online, known and new.
- **LTE data usage:** the [`huawei-trafficd`](../huawei-trafficd/README.md) dashboard at `/traffic.html`. The card also shows a one-line summary of the current LTE session, read from the anonymous `huawei.traffic` ubus object. If the daemon isn't running, that line just stays empty.

It is a single 5 KiB file with no external resources, styled like the traffic dashboard, with light and dark mode following the device setting.

## `cgi-bin/routerinfo`

A read-only shell script that the router's web server (uhttpd) runs as a CGI at `/cgi-bin/routerinfo`. It returns:

```json
{"uptime":1991126,"mem_total_kb":28580,"mem_available_kb":12056,"flash_total_kb":576,"flash_free_kb":244}
```

- **`uptime`:** seconds, from `/proc/uptime`.
- **`mem_available_kb`:** `MemAvailable` from `/proc/meminfo`. That's the memory programs can actually get, including cache the kernel would free, so it's more meaningful than "free".
- **Flash:** `df -k /overlay` for the writable flash.

It reads no input (any query string is ignored), so there's nothing to inject, and it needs no rpcd ACL. Each request starts a small shell process: about 50 ms, measured from the LAN.

## Install

From the workspace root (see [`../router-ssh/README.md`](../router-ssh/README.md)):

```sh
./router-ssh/copy www/index.html www/cgi-bin/routerinfo router:/tmp/
./router-ssh/connect 'cp /tmp/index.html /www/index.html && cp /tmp/routerinfo /www/cgi-bin/routerinfo && chmod 755 /www/cgi-bin/routerinfo && rm /tmp/index.html /tmp/routerinfo'
```

## Restore the original redirect

The firmware's original page is still in the read-only root filesystem at `/rom/www/index.html`:

```sh
./router-ssh/connect 'cp /rom/www/index.html /www/index.html; rm -f /www/cgi-bin/routerinfo'
```
