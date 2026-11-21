# gemgate: Gemini-to-HTTP gateway

Lets devices that can only speak HTTP browse [Gemini](https://geminiprotocol.net/) through the router. It was made for a Gemini browser on the Samsung TV (a Tizen 2.4 web app can't open TLS connections of its own). It's a CGI program for uhttpd, installed as **`/cgi-bin/gemini`**:

```
GET http://192.168.0.254/cgi-bin/gemini?url=<percent-encoded gemini:// URL>[&trust=<sha256 fingerprint>]
```

| | |
|---|---|
| Program | `gemgate`, 107 KB stripped, BearSSL linked statically; needs nothing beyond uClibc |
| Speed | 0.5–1.3 s per page over the LTE link (TCP and TLS handshake included); certificates already known: about 0.5 s |
| Flash | the program, plus `/srv/gemgate/known_hosts`, about 90 bytes per host |
| Installed | 2026-11-21 in the writable area; in `firmware/build.sh` from the next build on |

## What it answers

**When the Gemini server answered**, whatever its status, the HTTP status is `200` and the Gemini answer is in headers:

| Header | Content |
|---|---|
| `X-Gemini-Status` | Gemini's two-digit status (`20`, `10`, `31`, `51`…) |
| `X-Gemini-Meta` | the meta line, percent-encoded like `encodeURIComponent` (it may be UTF-8: input prompts, error messages); decode with `decodeURIComponent` |
| `X-Gemini-Cert` | SHA-256 fingerprint of the server's certificate |
| `X-Gemini-Cert-Status` | `new` (first visit, now stored), `known`, or `replaced` (see below) |
| `Content-Type` | for `2x`: the meta (`text/gemini` if empty); otherwise `text/plain` with an empty body |

For `2x` answers the body follows, **streamed through unbuffered**: any size, without using the router's RAM. Images and other files can be shown or downloaded directly from the gateway URL. Status `10`/`11` (input), `3x` (redirects) and `4x`–`6x` (failures) are passed on for the client to handle. The gateway doesn't follow redirects itself.

**When the gateway couldn't get an answer**, the HTTP status says so, and `X-Gateway-Error` names the reason; the body is a short explanation:

| HTTP | `X-Gateway-Error` | Cause |
|---|---|---|
| 400 | `bad-request`, `bad-url` | no `url`; not `gemini://`; user info, spaces, IPv6 literal or non-ASCII host; over 1024 bytes |
| 403 | `blocked-address` | the host resolves to a local or private address (see below) |
| 405 | `bad-request` | method other than GET or HEAD (OPTIONS answers CORS) |
| 502 | `dns`, `connect`, `bad-response` | no such host; connection failed; invalid Gemini header |
| 502 | `tls` | handshake failed; `X-Gateway-Detail: bearssl-<n>` has BearSSL's error number |
| 502 | `cert-changed` | the certificate differs from the stored one (see below) |
| 504 | `timeout` | no connection within 10 s, or no data for 20 s |

Every answer carries `Access-Control-Allow-Origin: *` (and exposes the headers above), `Cache-Control: no-store`, and `Content-Security-Policy: sandbox`.

## Certificates: trust on first use

Gemini sites mostly use self-signed certificates, so, like most Gemini clients, the gateway doesn't check CA signatures, names or dates. It pins each server's certificate instead:
- **First visit:** the SHA-256 fingerprint of `host:port`'s certificate is stored in `/srv/gemgate/known_hosts` (`host:port fingerprint first-seen`). `X-Gemini-Cert-Status: new`.
- **Same certificate later:** `known`.
- **Different certificate:** refused with `502`, `X-Gateway-Error: cert-changed`, and both fingerprints in `X-Gemini-Cert` (presented) and `X-Gemini-Cert-Known` (stored). If the change is expected (sites do renew certificates), repeat the request with `&trust=<presented fingerprint>`: it replaces the stored one (`replaced`, and a line in the system log).

To forget a host, delete its line from `known_hosts`. Future firmware upgrades that keep settings keep `/srv/gemgate/` (listed in the image's `keep.d`).

## Protocol details

- **TLS 1.2 only,** with SNI. Gemini requires 1.2 or later, and BearSSL has no TLS 1.3. On 2026-11-21 all servers tried accepted 1.2: `gemini.hjalves.dev`, `geminiprotocol.net`, `kennedy.gemi.dev`, `gemi.dev`, `station.martinrue.com`, `bbs.geminispace.org`, with ECDSA and RSA (4096-bit) keys. A server that insists on 1.3 fails with `tls`, and the message says why.
- **The request** is the URL without its `#fragment`, plus CRLF, sent to port 1965 unless the URL gives another. Host names are lower-cased; only IPv4 (the router has no IPv6).
- **The answer header** must be two digits, a space and a meta of at most 1024 bytes. A connection closed without TLS's `close_notify` after the body is accepted as the end, as most clients do.

## Safety

- **Only `gemini://`,** so it can't be used as a general web proxy.
- **No access to the LAN:** hosts that resolve to loopback, private (`10/8`, `172.16/12`, `192.168/16`), link-local, CGNAT (`100.64/10`), multicast or reserved addresses are refused, so it can't be used to reach devices on this network. (For local testing on a development machine, `GEMGATE_ALLOW_PRIVATE=1` in the environment lifts this.)
- **Sandboxed answers:** `Content-Security-Policy: sandbox` makes a browser treat everything served through the gateway as coming from nowhere. A remote HTML file opened directly through it can't run scripts on the router's site, for example to call `/ubus`.
- **No login,** like the dashboards: any LAN device may use it.

## Building

```sh
gemgate/fetch-bearssl.sh                    # BearSSL into gemgate/bearssl/ (git-ignored), pinned commit
QEMU_LD_PREFIX=/opt/x86_64-root make -C gemgate
```

The Makefile builds BearSSL for MIPS (`bearssl/build-mips/libbearssl.a`) on first use, then links `gemgate` against it with unused code removed (`--gc-sections`). BearSSL is pinned to commit `7bea48e` of its main branch (April 2026) rather than the last release, v0.6 from 2018, because of later security fixes: two P-256 carry-propagation bugs, an RSA-PSS verification bug, and a buffer overflow in key decoding.

For testing on the build host, the same source builds natively against a native BearSSL (`make -C bearssl lib`), with a test directory for the known hosts:

```sh
gcc -std=gnu99 -O2 -Ibearssl/inc -DKNOWN_DIR='"/tmp/gemgate-test"' gemgate.c bearssl/build/libbearssl.a -o /tmp/gemgate
QUERY_STRING='url=gemini%3A%2F%2Fgemini.hjalves.dev%2F' REQUEST_METHOD=GET /tmp/gemgate
```

## Installing

```sh
./router-ssh/copy gemgate/gemgate router:/www/cgi-bin/gemini
./router-ssh/connect 'chmod 755 /www/cgi-bin/gemini'
```

## Tested (2026-11-21)

- **Natively on the build host:**
  - **Statuses:** pages (`20`), an input request (`10`, kennedy.gemi.dev's search), a missing page (`51`) and a redirect (`31`) were passed on correctly, with ECDSA and RSA servers.
  - **Fingerprint:** the stored fingerprint matched `openssl x509 -fingerprint -sha256`.
  - **Certificate change:** after changing the stored fingerprint, the next request was refused with `cert-changed`; a wrong `trust=` was refused too; the right one replaced the entry; later requests were `known` again.
  - **Refused:** `http://` URLs, user info, `192.168.0.254`, `localhost`; an unknown host gave `dns`; POST gave `405`; OPTIONS gave `204`.
  - **Body:** the 30,459-byte Gemini specification was byte-identical to a direct fetch with `openssl s_client`.
- **On the router, through uhttpd, with `curl` from the LAN:** five public capsules loaded, at 0.6–1.3 s on first visit and about 0.5 s afterwards. The specification was again byte-identical.
