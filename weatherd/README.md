# weatherd: local weather station history

Receives the weather station's uploads on the router and keeps their history: the station's readings stay on the LAN and need no internet. The page is **http://192.168.0.254/weather.html** (also linked from the landing page), and the data is on ubus as `weather`.

| | |
|---|---|
| Daemon | `weatherd`, 36 KiB stripped |
| Links to | only libraries already on the router: `libubus`, `libubox`, `libm`, uClibc |
| Page | `/www/weather.html`, 28 KB (10 KB gzipped) |
| RAM | about 0.9 MiB RSS at start, much of it shared libraries. The full history adds about 600 KiB as it fills: 44 bytes per sample, 4320 uploads and 9600 hourly summaries |
| Flash | program, page and settings about 36 KiB on install; history about 21 bytes per hour, so about 180 KiB a year and at most about 190 KiB for the 13 months kept |
| Installed | 2026-11-17, enabled at boot (`/etc/init.d/weatherd`) |

## The station

A Fine Offset **WS2900** console (EasyWeather 1.7.6, model `WS2900_V2.01.18`, at `192.168.0.200`) with its **customized upload** set to the Ecowitt protocol, server `192.168.0.254`, port `8080`, path `/`, every 60 s. It then posts a form about every 64 seconds:
- outdoor and indoor temperature and humidity;
- relative and absolute pressure;
- wind speed, gust and direction;
- rain rate and rain counters (event, hour, day, week, month, year, total);
- solar radiation and UV index;
- the outdoor sensor's battery flag;
- the station's `PASSKEY`, an ID derived from its MAC address.

Values arrive in US units (°F, inHg, mph, in). weatherd stores them in metric:

| Reading | Stored as (integer) | From |
|---|---|---|
| Temperature (outdoor, indoor) | 0.1 °C | `tempf`, `tempinf` |
| Humidity (outdoor, indoor) | % | `humidity`, `humidityin` |
| Pressure (relative = sea level, absolute) | 0.1 hPa | `baromrelin`, `baromabsin` |
| Wind speed, gust | 0.1 m/s (the page shows km/h) | `windspeedmph`, `windgustmph` |
| Wind direction | degrees, where the wind comes from | `winddir` |
| Rain | 0.01 mm | `totalrainin` (see below) |
| Rain rate | 0.1 mm/h | `rainratein` |
| Solar radiation | W/m² | `solarradiation` |
| UV index, battery low | as sent | `uv`, `wh65batt` |

**Rain** is worked out from the station's never-resetting **total** counter. Its hourly, daily and other counters reset on the station's own schedule, so they aren't used. Each sample stores the rise since the previous upload. If the counter goes down (station reset or battery change), or rises implausibly fast, weatherd starts counting from the new value without recording any rain, and logs it. "Implausibly fast" means more than 30 mm plus 2 mm per minute since the previous upload, capped at 500 mm. So even a downpour, or a gap of several hours in uploads, still counts fully.

## Which uploads are accepted

- **Method:** only `POST` requests, at most 4 KB, completed within 5 seconds.
- **Sender:** only from the configured station address (`station`), and only with its `PASSKEY` (`passkey`). Either check is skipped if left empty. On the router both are set: the passkey was captured from one of the station's own uploads and written straight into `/etc/config/weatherd`, so it isn't stored anywhere else.
- **Values:** checked against physical ranges (−73 to 93 °C, 15 to 35 inHg, and so on). Anything out of range or not a number is treated as missing.
- **Retries:** an upload within 10 s of the previous one is answered `200` but ignored.
- **Without a clock:** uploads are answered `200` but not stored until the router's clock is set (NTP).

This is plain HTTP on the LAN, as the station sends it: the address and passkey checks keep out mistakes and casual fakes, not a determined attacker on the LAN.

## History

| Tier | Keeps | Default size | RAM when full |
|---|---|---|---|
| `fine` | every upload | 4320 = about 3 days | about 185 KiB |
| `coarse` | one summary per hour | 9600 = about 13 months | about 410 KiB |

**Hourly summary:**
- **Temperature:** average, lowest and highest.
- **Humidity, pressure, indoor readings:** averages.
- **Wind:** average speed, highest gust, and direction as a vector average, so readings of 350° and 10° average to 0°, not 180°.
- **Rain:** total for the hour, plus the highest rate.
- **Sun:** average and peak radiation, highest UV.
- **Battery:** "low" if any upload said so.

**Storage, as in huawei-trafficd:**
- **RAM tiers:** saved to `/tmp/weatherd.dat` (and `.coarse`) on stop, and loaded at start. That survives service restarts but not reboots.
- **Hourly summaries on flash,** in `/srv/weatherd/YYYY-MM.log`. Finished hours are appended once a day (`flash_every`) and on a clean stop, as compressed, delta-encoded records of about 21 bytes. 13 monthly files are kept; older ones are deleted whole.
  - A power cut loses at most a day of hourly data, plus the per-upload data.
  - Flash writes are rare and append-only, so wear is negligible.
  - A folder in RAM is refused, because it wouldn't persist.
  - Nothing is written if the overlay has less than 64 KiB free.
- **Firmware upgrades:** `/srv/weatherd/` is on the router's keep list (`/etc/sysupgrade.conf`, and in the firmware image's `keep.d`), so a flash with "keep settings" carries the history over.

**Flash format** (little-endian):
- **Block:** a 12-byte header (`"WB"`, version 1, record count, payload length as u16, 2 zero bytes, CRC-32 of the payload), then up to 255 records. The first record of a block is encoded against an "all unknown" sample, so every block decodes on its own, and a torn write loses only that block.
- **Record:** a flags byte, then varints for:
  - the time step minus the period;
  - the upload count;
  - the hour's rain;
  - the rain counter's change minus that rain.

  Then come groups present only when their flag is set, mostly as changes from the previous record:
  - `0x01` temperature: average change, average − min, max − average;
  - `0x02` indoor: temperature change and humidity byte;
  - `0x04` outdoor humidity byte;
  - `0x08` pressure: relative and absolute changes;
  - `0x10` wind: average, gust − average, direction change (−180…179);
  - `0x20` rain rate;
  - `0x40` sun: average, peak − average, UV byte;
  - `0x80` sensor battery low.

## ubus

| Method | Arguments | Returns |
|---|---|---|
| `latest` | none | the newest upload |
| `query` | `from`, `to` (Unix time; negative = seconds before now), `limit` (default 1000, at most 4000), `tier` (`auto`, `fine` or `coarse`) | samples, oldest first. `auto` gives hourly summaries before the fine tier begins, then every upload. `truncated: true` means more remain: continue from the last time + 1 |
| `status` | none | port, station address, whether the passkey is checked, station model and type, both tiers' sizes and time spans, accepted and rejected uploads, last error, the units below, and the flash history's state |

Sample fields, as scaled integers, since this ubus has no floating point:
- **Temperature, 0.1 °C:** `temp`, `temp_min`, `temp_max`, `temp_in`.
- **Humidity, %:** `hum`, `hum_in`.
- **Pressure, 0.1 hPa:** `press`, `press_abs`.
- **Wind:** `wind` and `gust` in 0.1 m/s; `dir` in degrees.
- **Rain:** `rain` and `rain_total` in 0.01 mm; `rain_rate` in 0.1 mm/h.
- **Sun:** `solar` and `solar_max` in W/m²; `uv`.
- **Other:** `batt_low`, `time`, and `n` (uploads behind the sample).

Fields with no reading are left out. An rpcd ACL allows **anonymous read** of these three methods, like the other dashboards. Nothing can be changed through ubus.

## Page

`weather.html`, styled like the LTE dashboard:
- **Tiles:**
  - temperature, with today's low and high;
  - humidity, with the dew point;
  - pressure, with its change over 3 hours;
  - wind speed, with gusts and a direction arrow;
  - rain today, with the current rate;
  - sun and UV;
  - indoor temperature and humidity;
  - the outdoor sensor's battery: **OK** (green dot) or **Low** (red dot), from the station's `wh65batt` flag (1 = low).

  They form a 4 × 2 grid, or 2 columns on phones.
- **Ranges:** presets for 6 h, 24 h, 3 d, 7 d, 30 d and 1 y, plus a custom range. The period grows with the range, from 5 minutes to a week, and days and weeks follow the local calendar.
- **Charts:**
  - temperature, with a lowest-to-highest band;
  - humidity;
  - pressure;
  - wind, with a band up to the strongest gust;
  - rain as bars;
  - sunshine, with a band up to the peak.

  They're one hue, single-series, with a shared hover crosshair and tooltip (also with ← → on a focused chart).
- **CSV download** of the raw samples, in metric.

Light and dark mode.

## Configuration

`/etc/config/weatherd`:

| Option | Default | |
|---|---|---|
| `port` | `8080` | TCP port the station posts to |
| `station` | `192.168.0.200` | accepted sender address; empty = any |
| `passkey` | (empty) | required `PASSKEY`; empty = not checked |
| `history` | `4320` | uploads kept in RAM |
| `coarse_interval`, `coarse_history` | `3600`, `9600` | summary period (s) and how many are kept |
| `state_file` | `/tmp/weatherd.dat` | RAM tiers' file |
| `save_every` | `0` | also save the RAM tiers every N s |
| `flash_dir`, `flash_every` | `/srv/weatherd`, `86400` | hourly history on flash; empty = off |

## Build and install

```sh
make -C weatherd                     # OpenWrt 15.05.1 SDK in ../toolchain
./router-ssh/copy weatherd/weatherd weatherd/files/weatherd.* weatherd/www/weather.html router:/tmp/
```

On the router:

```sh
cd /tmp
cp weatherd /usr/sbin/ && cp weatherd.init /etc/init.d/weatherd && chmod 755 /usr/sbin/weatherd /etc/init.d/weatherd
cp weatherd.config /etc/config/weatherd && cp weatherd.acl.json /usr/share/rpcd/acl.d/weatherd.json && cp weather.html /www/
uci set weatherd.main.passkey='<the station PASSKEY>' && uci commit weatherd
/etc/init.d/weatherd enable && /etc/init.d/weatherd start
/etc/init.d/rpcd restart            # loads the ACL; logs out admin sessions
```

The firmware build (`firmware/build.sh`) includes weatherd, its page and ACL from the next image on.

**Uninstall:** `/etc/init.d/weatherd stop; /etc/init.d/weatherd disable; rm -r /usr/sbin/weatherd /etc/init.d/weatherd /etc/config/weatherd /usr/share/rpcd/acl.d/weatherd.json /www/weather.html /srv/weatherd; /etc/init.d/rpcd restart`

## Verification performed (2026-11-17)

- **Capture:** two real uploads were captured on the router with a temporary listener, to learn the format and interval.
- **Native test,** with AddressSanitizer and UBSan, against a fake station that posts exactly that format. No sanitizer reports.
  - **Conversions:** checked against hand calculation, for example 57.6 °F → 14.2 °C, 29.997 inHg → 1015.8 hPa, 0.9 mph → 0.4 m/s and 4.291 in → 108.99 mm.
  - **Rain:**
    - 0.01 in steps gave 0.25 mm;
    - a counter reset started a new baseline with no false rain;
    - an implausible 127 mm jump between uploads a second apart was rejected.
  - **Rejected requests:** wrong passkey (403), wrong sender (403), GET (405), oversized (413), and a client that stalls (dropped after 5 s). An upload whose sender closed early was still accepted.
  - **Hourly summaries,** with a 12-second test "hour": averages, extremes, the vector-averaged direction, rain sums and peaks.
  - **Flash:** after a stop and a restart from the flash history alone, every finished period read back **identical**.
  - **Fuzzing:** 3,000 random malformed uploads (bad numbers, `nan`, huge values, broken escapes, random bytes), with no crash.
- **Flash size:** a Python copy of the encoder, run over a synthetic year of smooth hourly weather, gave 20.7 bytes per hour, block headers included (177 KiB a year). The router's JFFS2 compresses it further.
- **Page:** checked against a mock with 13 months of hourly and 3 days of per-upload data. Every preset, the tooltip, keyboard navigation, light and dark mode and a 390 px phone layout (no sideways scrolling) worked, with no script errors.
- **On the router:**
  - started with the station address and passkey checks on;
  - the station's uploads are accepted;
  - first real reading: 14.3 °C and 99 % outside, 1016.0 hPa, raining at 1.8 mm/h;
  - the landing card and `weather.html` show it, with no script errors.
