# Router landing page

`index.html` replaces the router's default `/www/index.html`, which only redirects to LuCI. It is served at **http://192.168.0.254/** and links to:

- **Router admin:** LuCI at `/cgi-bin/luci`. This LuCI version serves its login form with HTTP status 403; that is normal.
- **LTE data usage:** the [`huawei-trafficd`](../huawei-trafficd/README.md) dashboard at `/traffic.html`. The card also shows a one-line summary of the current LTE session, read from the anonymous `huawei.traffic` ubus object. If the daemon isn't running, that line just stays empty.

It is a single 3 KiB file with no external resources, styled like the traffic dashboard, with light and dark mode following the device setting.

## Install

From the workspace root (see [`../router-ssh/README.md`](../router-ssh/README.md)):

```sh
./router-ssh/copy www/index.html router:/tmp/index.html
./router-ssh/connect 'cp /tmp/index.html /www/index.html && rm /tmp/index.html'
```

## Restore the original redirect

The firmware's original page is still in the read-only root filesystem at `/rom/www/index.html`:

```sh
./router-ssh/connect 'cp /rom/www/index.html /www/index.html'
```
