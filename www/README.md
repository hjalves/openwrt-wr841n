# Router landing page

`index.html` replaces the router's default `/www/index.html`, which only redirects to LuCI. It is served at **http://192.168.0.254/** and links to:

- **Settings:** the [admin page](../admin/README.md) at `/admin.html`. LuCI (`/cgi-bin/luci`) is still linked in small print below the cards; this LuCI version serves its login form with HTTP status 403, which is normal. The card also shows the router's **uptime, memory available and flash free**, from `cgi-bin/routerinfo` (below).
- **Devices on the network:** the [`lanwatch`](../lanwatch/README.md) page at `/devices.html`. The card shows how many devices are online, known and new.
- **Weather:** the [`weatherd`](../weatherd/README.md) page at `/weather.html`. The card shows the current temperature, humidity, pressure and wind.
- **LTE connection:** the [`huawei-trafficd`](../huawei-trafficd/README.md) dashboard at `/traffic.html`. The card also shows a one-line summary of the current LTE session, read from the anonymous `huawei.traffic` ubus object. If the daemon isn't running, that line just stays empty.

It is a single 5 KiB file with no external resources, styled like the traffic dashboard, with light and dark mode following the device setting.

## Icons

All three pages (this one, `traffic.html` and `devices.html`) use the same icon: white signal bars on a blue (`#2a78d6`) rounded square.

| File | Size | Used by |
|---|---|---|
| `favicon.svg` | 387 B | Browser tabs in Chrome, Firefox and Edge (sharp at any size) |
| `favicon-32.png` | 396 B | Browsers without SVG icon support, such as Safari |
| `apple-touch-icon.png` | 1.4 KB | iOS/iPadOS Safari Favorites and Home Screen: 180×180, opaque and square (iOS rounds the corners itself) |

All three show the same symbol, a router with Wi-Fi arcs in white on the page blue (since 2026-11-21; before, the favicons showed signal bars). The favicons are a version simplified for 16–32 px: a larger router body without status lights, two thick arcs, and rounded corners, drawn on a 32-unit grid so the shapes land on whole pixels. `apple-touch-icon.png` is the detailed version, flat and quantised to 32 colours. The PNGs were rendered with headless Chromium. iOS fetches the touch icon from the site root by itself, so the pages don't need to link it.

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
./router-ssh/copy www/index.html www/cgi-bin/routerinfo www/favicon.svg www/favicon-32.png www/apple-touch-icon.png router:/tmp/
./router-ssh/connect 'cd /tmp && cp index.html favicon.svg favicon-32.png apple-touch-icon.png /www/ && cp routerinfo /www/cgi-bin/routerinfo && chmod 755 /www/cgi-bin/routerinfo && rm index.html routerinfo favicon.svg favicon-32.png apple-touch-icon.png'
```

## Restore the original redirect

The firmware's original page is still in the read-only root filesystem at `/rom/www/index.html`:

```sh
./router-ssh/connect 'cp /rom/www/index.html /www/index.html; rm -f /www/cgi-bin/routerinfo /www/favicon.svg /www/favicon-32.png /www/apple-touch-icon.png'
```
