# SSH access to the router

Run from the workspace root:

```sh
./router-ssh/connect
```

Enter the router password at the SSH prompt. It is not stored in this project or passed on the command line. Exit the remote shell with `exit` or Ctrl+D.

To run a command instead of opening a shell:

```sh
./router-ssh/connect 'uname -a'
```

Docker is required. The first run builds a small Debian/OpenSSH client image; later runs reuse it. The container uses host networking to reach the router. The image can be rebuilt after editing the Dockerfile with `docker build --network host -t opencode-openwrt-router-ssh:local router-ssh`.

## Host-key verification

On the first connection, SSH asks you to verify the router's host key. The RSA fingerprint observed during the inventory was:

```text
SHA256:jjM7WpR5nlKuqqVZKJaW3YSbk3cZWBOHqVBxKPrSmis
```

That value was observed over the same unverified network, so **verify it through an independent trusted channel** before accepting it. Accepted host keys are saved under `${XDG_STATE_HOME:-$HOME/.local/state}/openwrt-router-ssh/known_hosts` and checked on later connections. If the key changes, investigate rather than automatically accepting it. The first connection requires an interactive terminal to answer the verification prompt.

## Why Docker?

The router's Dropbear 2015.67 requires legacy SSH algorithms. The helper enables `diffie-hellman-group14-sha1` and `ssh-rsa` only for this connection, using an older OpenSSH client that was tested with the router. It does not weaken your workstation's system-wide SSH configuration. These algorithms are obsolete; use the helper only on a trusted management network.
