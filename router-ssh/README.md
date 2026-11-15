# SSH access to the router

Helpers for the OpenWrt router (`192.168.0.254`), using the workstation's own OpenSSH client. No Docker or extra packages are needed. Run them from the workspace root:

| Script | Purpose |
|---|---|
| `./router-ssh/connect` | Interactive shell, or `./router-ssh/connect 'command'` to run a command |
| `./router-ssh/copy SRC... DEST` | Copy files; write router paths as `router:/path` |
| `./router-ssh/tunnel [LOCAL [REMOTE]]` | Forward `127.0.0.1:LOCAL` (default 18080) to the router's own `127.0.0.1:REMOTE` (default 80, uhttpd) |

```sh
./router-ssh/connect
./router-ssh/connect 'logread | tail -20'
./router-ssh/copy build/prog router:/tmp/demo/
./router-ssh/copy router:/etc/config/network ./network.backup
./router-ssh/tunnel            # then use http://127.0.0.1:18080/ ; Ctrl+C to stop
```

Enter the router password when prompted. Exit the remote shell with `exit` or Ctrl+D.

`ROUTER_HOST` and `ROUTER_USER` override the defaults (`192.168.0.254`, `root`).

### Non-interactive use

If `sshpass` is installed and the `SSHPASS` environment variable is set, the helpers take the password from that variable. It never appears on a command line, so it doesn't show up in `ps`:

```sh
read -rs SSHPASS && export SSHPASS     # type the password; it isn't echoed or saved in history
./router-ssh/connect 'uptime'
```

Unset it when finished (`unset SSHPASS`).

## Why the special options

The router runs Dropbear 2015.67, which only offers old algorithms:

- key exchange: `diffie-hellman-group14-sha1`, `diffie-hellman-group1-sha1`
- host keys: `ssh-rsa`, `ssh-dss`
- no SFTP server

Modern OpenSSH (tested with 10.6) still implements `diffie-hellman-group14-sha1` and `ssh-rsa` but disables them by default. `common.sh` enables them **for these connections only**, using `-o` options, and does not change the workstation's system-wide SSH configuration. `copy` passes `-O` to `scp` so it uses the old SCP protocol, because current `scp` uses SFTP by default.

Two details that matter:

- **Set `KexAlgorithms` to exactly `diffie-hellman-group14-sha1`.** Adding it to OpenSSH's default list (`+diffie-hellman-group14-sha1`) still negotiates that algorithm, but the connection then hangs waiting for the router's key-exchange reply. The cause wasn't investigated.
- **Password authentication only.** Public-key login is turned off. Dropbear 2015 doesn't support Ed25519 keys, and RSA keys would also need `ssh-rsa` signatures enabled.

These algorithms are obsolete; use the helpers only on a trusted network.

## Host-key verification

On the first connection, SSH asks you to verify the router's host key. The RSA fingerprint is:

```text
SHA256:jjM7WpR5nlKuqqVZKJaW3YSbk3cZWBOHqVBxKPrSmis
```

It was observed on 2026-11-14 and again on 2026-11-15, but both times over the same LAN. **Check it through an independent trusted channel** (for example, on the router's console: `dropbearkey -y -f /etc/dropbear/dropbear_rsa_host_key`) before accepting it.

Accepted keys are saved in `${XDG_STATE_HOME:-$HOME/.local/state}/openwrt-router-ssh/known_hosts` and checked on every later connection. If the key changes, investigate before accepting the new one. The first connection needs an interactive terminal to answer the verification prompt.
