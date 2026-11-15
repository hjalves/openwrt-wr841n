# Shared settings for the router SSH helpers. Sourced, not executed.
#
# Dropbear 2015.67 on the router only offers diffie-hellman-group14-sha1 /
# group1-sha1 key exchange and ssh-rsa / ssh-dss host keys. Modern OpenSSH
# still implements these but disables them by default, so they are enabled
# here for these connections only.
#
# Note: KexAlgorithms must be set to exactly this algorithm. Appending it to
# OpenSSH's default list ("+diffie-hellman-group14-sha1") negotiates the same
# algorithm but then hangs waiting for the server's key exchange reply.

router_host=${ROUTER_HOST:-192.168.0.254}
router_user=${ROUTER_USER:-root}
state_dir=${XDG_STATE_HOME:-"$HOME/.local/state"}/openwrt-router-ssh

umask 077
mkdir -p "$state_dir"
touch "$state_dir/known_hosts"

set -- \
    -o KexAlgorithms=diffie-hellman-group14-sha1 \
    -o HostKeyAlgorithms=ssh-rsa \
    -o UserKnownHostsFile="$state_dir/known_hosts" \
    -o StrictHostKeyChecking=ask \
    -o UpdateHostKeys=no \
    -o PubkeyAuthentication=no \
    -o PreferredAuthentications=password \
    -o ConnectTimeout=10 \
    "$@"

# Run ssh/scp with the options above. If SSHPASS is set and sshpass is
# installed, the password is taken from that variable (never from argv);
# otherwise ssh prompts for it.
router_exec() {
    tool=$1
    shift
    if [ -n "${SSHPASS:-}" ] && command -v sshpass >/dev/null 2>&1; then
        exec sshpass -e "$tool" "$@"
    fi
    exec "$tool" "$@"
}
