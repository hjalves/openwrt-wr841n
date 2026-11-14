# Persistent C service through uhttpd and ubus

This example lets the router's existing uhttpd handle HTTP, without CGI or a separate HTTP listener:

```text
HTTP client -> uhttpd /ubus -> ubus -> ubus-hello C process
```

The process registers the `example.hello` object with a no-argument `say` method. It stays running in the `uloop` event loop and returns `{ "message": "Hello World" }` for each call. uhttpd's ubus bridge uses **JSON-RPC POST requests**, not arbitrary routes such as `GET /hello`.

For the router's currently discovered JSON-RPC operations and ubus methods, see [UBUS_METHODS.md](UBUS_METHODS.md).

## Files

- `ubus-hello.c`: C service, using the router's existing `libubus` and `libubox`.
- `Makefile`: cross-build with the OpenWrt 15.05.1 SDK in `../toolchain/`.
- `hello-example.acl.json`: optional rpcd ACL for authenticated HTTP calls; not installed automatically.
- `client.py`: a host-side Python 3 HTTP client that prompts for the password and uses a local SSH tunnel.

## Build

From the workspace root:

```sh
make -C ubus-hello
```

The SDK must be at the path specified at the top of the Makefile. To use another SDK location:

```sh
make -C ubus-hello SDK=/absolute/path/to/extracted-sdk
```

The output `ubus-hello/ubus-hello` is dynamically linked for the router's MIPS/uClibc environment. No software is installed on the router by the build.

## Transfer and run temporarily

These commands use the workspace's `router-ssh/connect` Docker helper and require Docker. First connect interactively and independently verify the router's host-key fingerprint as described in [`../router-ssh/README.md`](../router-ssh/README.md). The helper builds its client image and remembers the accepted key.

Create a new temporary directory on the router. `mkdir` fails rather than overwriting an existing directory:

```sh
./router-ssh/connect 'mkdir -m 700 /tmp/ubus-hello-demo'
```

From the workspace root, copy the executable and ACL example using the same Docker image and known-hosts file:

```sh
STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/openwrt-router-ssh"
docker run --rm -it --network host \
  --mount "type=bind,src=$PWD/ubus-hello,dst=/app,readonly" \
  --mount "type=bind,src=$STATE_DIR,dst=/root/.ssh" \
  --entrypoint scp opencode-openwrt-router-ssh:local \
  -O -o KexAlgorithms=diffie-hellman-group14-sha1 \
  -o HostKeyAlgorithms=ssh-rsa -o StrictHostKeyChecking=ask \
  /app/ubus-hello /app/hello-example.acl.json \
  root@192.168.0.254:/tmp/ubus-hello-demo/
```

Open an interactive router shell, then run the service in the foreground:

```sh
./router-ssh/connect
```

On the router:

```sh
chmod 700 /tmp/ubus-hello-demo/ubus-hello
/tmp/ubus-hello-demo/ubus-hello
```

In a second terminal, call it locally over SSH:

```sh
./router-ssh/connect "ubus call example.hello say '{}'"
```

Expected response:

```json
{"message":"Hello World"}
```

Press Ctrl+C in the router shell to stop the service. Files under `/tmp` disappear on reboot. This is a demonstration, not a production supervisor: it does not automatically start at boot or reconnect if ubusd restarts. A production service should use `procd` and handle reconnection and errors.

## HTTP access and authorization

The router already exposes `/ubus`. A login request uses the all-zero session to call `session.login`; the returned session token is then supplied to `example.hello.say`. For example, the method call body is:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "call",
  "params": ["SESSION_ID", "example.hello", "say", {}]
}
```

The result is `{"jsonrpc":"2.0","id":1,"result":[0,{"message":"Hello World"}]}`. The first result element is the ubus status; `0` means success. HTTP status 200 alone does not prove that the RPC call succeeded: permission failures can appear as JSON-RPC errors.

**An authenticated session also needs an RPC ACL.** During the earlier test, the local ubus call succeeded but an authenticated HTTP call returned `Access denied` until a temporary session grant was added. The included ACL grants only `example.hello.say` to sessions authorized for the `hello-example` ACL group. The observed router root login configuration allowed all ACL groups for reading, but verify your current policy before relying on it.

If you deliberately want persistent authenticated HTTP access, review the ACL and install it as a **new** file at `/usr/share/rpcd/acl.d/hello-example.json`. Assuming you transferred the example as shown above, this command refuses to overwrite an existing file or symlink:

```sh
./router-ssh/connect '
  dest=/usr/share/rpcd/acl.d/hello-example.json
  if [ -e "$dest" ] || [ -L "$dest" ]; then
    echo "ACL already exists; not overwriting" >&2
    exit 1
  fi
  cp /tmp/ubus-hello-demo/hello-example.acl.json "$dest"
'
```

This writes to the router's flash; it is not part of running the local ubus example. Log in again after installing the ACL so the new session receives the permission. Nothing in this project installs or removes the ACL automatically. Stopping the demo does not remove an installed ACL; removing an ACL file later also does not necessarily revoke permissions from already-issued sessions.

### Use an SSH tunnel for the HTTP client

The router's HTTP endpoint does not use TLS. Do not send its password or a session token across an untrusted network. After accepting the router host key with the helper, start a tunnel from the workspace root:

```sh
STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/openwrt-router-ssh"
docker run --rm -it --network host \
  --mount "type=bind,src=$STATE_DIR,dst=/root/.ssh" \
  opencode-openwrt-router-ssh:local \
  -o KexAlgorithms=diffie-hellman-group14-sha1 \
  -o HostKeyAlgorithms=ssh-rsa -o StrictHostKeyChecking=ask \
  -o ExitOnForwardFailure=yes \
  -N -L 127.0.0.1:18080:127.0.0.1:80 root@192.168.0.254
```

Keep the tunnel running and, in another terminal, run:

```sh
python3 ubus-hello/client.py
```

The client prompts for the root password rather than storing it in the script or shell history. It requests a 60-second RPC session and prints the response without printing the session token. It requires Python 3 on the **computer**, not on the router. The service must be running and the ACL must already be installed for the HTTP call to work.

The router's `/ubus` method-list operation was observed to be accessible without authentication. Do not assume custom objects or methods are invisible; authorize each method narrowly, and keep this old firmware on a trusted management network.

## Verification performed

The same C service was cross-compiled and tested temporarily on this router. A local `ubus` call returned `Hello World`; an HTTP call returned `Access denied` before a temporary in-memory ACL grant and succeeded after it. The test session, process, and temporary files were removed. No persistent ACL, boot service, or router configuration was installed.
