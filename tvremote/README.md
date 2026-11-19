# tvremote: Samsung TV remote control

Switches the Samsung TV on and off, sends it remote-control keys, shows and sets its volume, and opens its apps, from the router. The page is **http://192.168.0.254/remote.html** (also linked from the landing page); the daemon is on ubus as `tv`.

| | |
|---|---|
| Daemon | `tvremote`, 25 KiB stripped |
| Links to | only libraries already on the router: `libubus`, `libubox`, `libblobmsg_json`, uClibc (no TLS needed) |
| Page | `/www/remote.html`, about 18 KB |
| RAM | about 1 MiB RSS, nearly all shared libraries |
| Installed | 2026-11-19 in the writable area, enabled at boot (`/etc/init.d/tvremote`); in `firmware/build.sh` from the next build on |

## The TV

A Samsung **UE32K5500** (2016, Tizen 2.4, `16_HAWKM_FHD`, `msfVersion` 2.1.0), wired, at `192.168.0.20`, MAC `f8:3f:51:76:91:85`. That address is set statically on the TV (Settings → General → Network → Network Status → IP Settings), outside the Huawei's DHCP pool (.200–.250): the Huawei can't reserve addresses, and the daemon doesn't look for the TV. (It was `192.168.0.212` by DHCP until 2026-11-19.)

- **Power on: Wake-on-LAN.** A magic packet (6 × `0xff`, then the MAC 16 times) as a UDP broadcast to `192.168.0.255`, port 9. It crosses the powerline link to the TV. The TV answers on port 8001 about 3–15 s later: quicker if it went to standby only recently.
- **Keys and power off: the remote-control WebSocket**, `ws://192.168.0.20:8001/api/v2/channels/samsung.remote.control?name=<base64 name>`. This model accepts plain `ws://` on port 8001 with **no token and no "Allow" prompt**. Port 8002 (`wss://`, token) is open too, but not needed. Each key is one JSON text frame:

  ```json
  {"method":"ms.remote.control","params":{"Cmd":"Click","DataOfCmd":"KEY_VOLUP","Option":"false","TypeOfRemote":"SendRemoteKey"}}
  ```

- **After `KEY_POWER`** the TV stays on the network for about 17 s. A second `KEY_POWER` in that window could switch it back on.

- **Apps, over the same WebSocket:**
  - **List:** `{"method":"ms.channel.emit","params":{"event":"ed.installedApp.get","to":"host"}}`. The TV answers with an `ed.installedApp.get` event listing each app's `appId`, `name` and `app_type` (about 4 KB for 16 apps).
  - **Launch:** `ed.apps.launch` with `{"appId": …, "action_type": …}`. The action is `DEEP_LINK` for web apps (`app_type` 2, nearly all) and `NATIVE_LAUNCH` for native ones (type 4: the Web Browser). The TV acknowledges with `"data": true`. A cold start takes up to about 9 s.
  - **REST alternative, not used:** `POST /api/v2/applications/<id>` (launch) and `DELETE` (close) on port 8001. In testing, the launch hung without an answer and the close failed for Netflix. `GET /api/v2/applications/<id>` (`running`, `visible`) is reliable.
  - **Leaving an app:** Exit or Home.
- **Sources:** there's no direct source command on this model. Its only remote-control UPnP service is `SendKeyCode`, and the older `SetMainTVSource` service is gone. Tested with someone watching:
  - `KEY_HDMI` cycles the HDMI inputs (HDMI 1, then HDMI 2);
  - `KEY_HDMI1` and `KEY_TV` do nothing;
  - `KEY_SOURCE` opens the source bar.

  Nothing reports the current input.
- **Volume: UPnP RenderingControl**, SOAP over HTTP at `http://192.168.0.20:9197/upnp/control/RenderingControl1`: `GetVolume`, `GetMute` and `SetVolume` (0–100, `Channel` `Master`). Setting is exact, not key presses. This is the TV's own volume: a soundbar or receiver on HDMI ARC or optical isn't covered.

Read-only TV information: `http://192.168.0.20:8001/api/v2/`.

## How the daemon works

It keeps the WebSocket open while the TV is on, so keys go out with no reconnect delay, and tracks the TV's power state:

```
off ──power_on (WoL)──▶ waking ──channel open──▶ on ──power_off (KEY_POWER)──▶ turning_off ──25 s──▶ off
```

- **off:** probes port 8001 every `poll` seconds (default 30), so switching the TV on with its own remote is noticed. A probe of a sleeping TV is one unanswered TCP SYN.
- **waking:** WoL sent; probes every 2 s, gives up after `wake_timeout` (40 s).
- **on:** connection open. TCP keepalive (10 s idle, then 3 probes 5 s apart) notices a TV that vanished. If the connection drops after it has worked for 10 s or more, the daemon reconnects at once; if that fails, the TV is off.
- **turning_off:** `KEY_POWER` sent. For `off_window` seconds (25) nothing is sent and the TV isn't probed. The socket is left open until then: closing it straight after the key can send a TCP reset, and the TV then drops the key it hasn't read yet. (That happened in testing.)

Requests during a transition aren't lost:
- **Keys while waking** (up to 8) are queued and sent once the channel is open.
- **Power on while switching off** sends the WoL when the window ends.
- **Power off while waking** sends `KEY_POWER` once the TV is up.
- **`KEY_POWER` via `key`** goes through the same power logic.

**Apps:**
- **The list** is asked for each time the TV comes up, and on an `apps` call while the list is still empty. It's kept while the TV is off, so the page can show it then too.
- **`launch`** looks up the app's type in the list, to choose `DEEP_LINK` or `NATIVE_LAUNCH`.
- **Launching while the TV is off** switches it on first (WoL) and launches when the channel opens. The same happens while it wakes or reconnects; only the latest app waits.
- **While it's switching off,** a launch is refused.

**Volume** is only asked for while the TV is on, and only when needed:
- when it comes on;
- when `status` is called and the last reading is 2 s old or more (the reply carries the previous reading; the next call gets the new one), so it's read only while someone is watching the page;
- 0.4 s after the last of a burst of `KEY_VOLUP`, `KEY_VOLDOWN` or `KEY_MUTE`;
- after each `set_volume`, to read back what the TV made of it.

One UPnP request runs at a time. Of several `set_volume` calls in quick succession (a dragged slider), only the latest waiting value is sent. After a failed request the volume isn't asked for again for 30 s, so a TV without the service doesn't fill the log.

If the TV ever sends `ms.channel.unauthorized` (a firmware that wants approval), the daemon logs it and `status` shows it in `last_error`. Allow "OpenWrt" on the TV under Settings → General → External Device Manager → Device Connection Manager.

## ubus `tv`

| Method | Arguments | Result |
|---|---|---|
| `status` | – | `state` (`off`, `waking`, `on`, `turning_off`), `since` (seconds in that state), `connected`, `then` (an `on`/`off` waiting for the current transition), `then_launch` (an app waiting for the TV), `volume` (0–100) and `mute` while on and known, `host`, `mac`, `name`, `keys_sent`, `wol_sent`, `launches`, `last_error` and `last_error_age` |
| `power_on` | – | `state`, `then` |
| `power_off` | – | `state`, `then` |
| `key` | `key`: `KEY_` followed by A–Z, 0–9 or `_`; `cmd`: `Click` (default), `Press` or `Release` | `state` and `sent: true`, or `queued: true` while the TV wakes |
| `apps` | – | `apps`: `[{id, name, type}]` in the TV's order, as the TV reports them; `age` (seconds since fetched) |
| `launch` | `app`: an app ID (letters, digits, `.`, `_`, `-`) | `state` and `sent: true`, or `queued: true` (switching on / waking) |
| `set_volume` | `volume`: 0–100 | `state` and `volume` (the request is sent in the background; `status` shows the TV's own reading after it) |

Errors: `key` and `set_volume` return status 9 (no data) while the TV is off or switching off (`launch` only while switching off), and 2 (invalid argument) for a malformed key or app ID, or a volume outside 0–100. `set_volume` returns 8 (not supported) if the volume is turned off (`upnp_port` 0). Any `KEY_` name is passed through as-is; which ones the TV knows is up to the TV.

```sh
ubus call tv power_on
ubus call tv key '{"key":"KEY_VOLUP"}'
ubus call tv set_volume '{"volume":12}'
ubus call tv apps
ubus call tv launch '{"app":"111299001912"}'      # YouTube
ubus call tv power_off
```

**Access:** all seven methods are open to the anonymous session (`files/tvremote.acl.json`), like the dashboards. A login wouldn't protect anything: the TV itself accepts remote-control connections from any LAN client without a token. rpcd only grants the anonymous session the `read` part of an ACL, so the control methods are listed under `read`. After installing or changing an ACL, `/etc/init.d/rpcd restart` (a `reload` doesn't rebuild the anonymous session; a restart logs out the admin page).

## The page

`remote.html` is made for a phone, in light and dark themes:
- **Top row:** Power, Source, HDMI (next HDMI input) and Menu.
- **Power:** one button. It shows "Turn on" or "Turn off" for the current state, and the header shows the TV's state (polled every 4 s, every 1 s during transitions).
- **Keys:** Source and Menu; the arrows and OK; Back, Home and Exit; volume, mute, Info and channel.
- **Apps panel:** a button per app, in the TV's order and with the TV's names (long ones cut with "…"; the full name on hover). It's foldable, and the page remembers whether it was open. A tap on an app while the TV is off switches it on and opens the app.
- **Volume panel** (while the TV is on and reports it): the current volume, or "Muted (n)" in red, and a 0–100 slider. A drag sends at most a few values a second, one request at a time, and the panel ignores the TV's readings for 2.5 s after you touch the slider, so it doesn't jump back under your finger. After the volume or mute keys, the page reads the new value about 0.7 s later.
- **Folded away:** playback keys and Guide, and the number keys.
- **Holding a key:** the arrows, volume and channel keys repeat while held.
- **Keyboard:** arrows, Enter, Backspace (back), Esc (exit), H (home), + and − (volume), M (mute), and the digits.

## Configuration (`/etc/config/tvremote`)

| Option | Default | |
|---|---|---|
| `host` | – (required) | The TV's IPv4 address |
| `mac` | – (required) | The TV's MAC, for Wake-on-LAN |
| `broadcast` | `255.255.255.255` | WoL destination; set to `192.168.0.255` |
| `port` | `8001` | The TV's remote-control port |
| `name` | `OpenWrt` | Name this remote gives the TV |
| `poll` | `30` | Seconds between probes while off; 0 = only when asked to switch on |
| `wake_timeout` | `40` | Give up waking after this |
| `off_window` | `25` | The TV's shutdown window after `KEY_POWER` |
| `upnp_port` | `9197` | The TV's UPnP port, for the volume; 0 = no volume |

## Building and installing

```sh
QEMU_LD_PREFIX=/opt/x86_64-root make -C tvremote      # on the ARM VM; see the top-level README
./router-ssh/copy tvremote/tvremote router:/usr/sbin/tvremote
./router-ssh/copy tvremote/files/tvremote.init router:/etc/init.d/tvremote
./router-ssh/copy tvremote/files/tvremote.config router:/etc/config/tvremote
./router-ssh/copy tvremote/files/tvremote.acl.json router:/usr/share/rpcd/acl.d/tvremote.json
./router-ssh/copy tvremote/www/remote.html router:/www/remote.html
./router-ssh/connect 'chmod 755 /usr/sbin/tvremote /etc/init.d/tvremote; /etc/init.d/tvremote enable; /etc/init.d/tvremote start; /etc/init.d/rpcd restart'
```

## Tested (2026-11-19)

- **Power on:** WoL from the router woke the TV; keys pressed while it woke were queued and then sent.
- **Keys:** volume, mute and others, both through ubus and by tapping the page in a headless phone browser.
- **Power off:** the TV really went off. Ping and port 8001 were gone after the window.
- **Power on during the shutdown window:** the WoL was held until the window ended, then the TV woke.
- **Anonymous access:** HTTP calls to `/ubus` work with the ACL.
- **Apps** (2026-11-19, later):
  - **List:** all 16 apps listed.
  - **Launching:** YouTube came to the foreground (a cold start takes about 9 s). The TV acknowledged the Web Browser launch (`NATIVE_LAUNCH`), but whether it appeared wasn't checked: the REST status doesn't cover the browser. An invalid app ID is refused.
  - **Launching while the TV was off:** it woke in 3 s, and YouTube was showing 9 s later.
  - **The page:** the apps panel and a launch tested in a headless phone browser.
  - **Sources:** tested by hand, as described above.
- **Volume** (2026-11-19, later): `status` matched the TV's own `GetVolume`/`GetMute` after every step: `set_volume`, volume keys, mute and unmute, a burst of 5 values (only the last sent). The page's slider and volume buttons were also tested in a headless phone browser, with the TV agreeing after each step.

**Not tested yet:**
- **Waking after many hours in standby.** If WoL stops working then, enable the TV's option to be switched on from a phone or over the network.
- **Switching off with the TV's own remote.** It should be noticed within about 25–45 s (keepalive, then a reconnect attempt).
- **Very long idle connections.** One stayed open for several minutes without trouble.
