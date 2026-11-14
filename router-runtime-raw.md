# Router development runtime: raw inventory

Read-only SSH inventory taken 2026-11-14. These are snapshots and paths may include network-sensitive information. No passwords, private-key contents, account hashes, or configuration values were requested.

## Shared libraries and symlinks

```text
-rwxr-xr-x    1 0        0            28972 Jan 31  2016 /lib/ld-uClibc-0.9.33.2.so
lrwxrwxrwx    1 0        0               21 Feb  2  2016 /lib/ld-uClibc.so.0 -> ld-uClibc-0.9.33.2.so
-rw-r--r--    1 0        0             6160 Jan 31  2016 /lib/libblobmsg_json.so
lrwxrwxrwx    1 0        0               21 Feb  2  2016 /lib/libc.so.0 -> libuClibc-0.9.33.2.so
-rwxr-xr-x    1 0        0             9712 Jan 31  2016 /lib/libcrypt-0.9.33.2.so
lrwxrwxrwx    1 0        0               20 Feb  2  2016 /lib/libcrypt.so.0 -> libcrypt-0.9.33.2.so
-rwxr-xr-x    1 0        0            12504 Jan 31  2016 /lib/libdl-0.9.33.2.so
lrwxrwxrwx    1 0        0               17 Feb  2  2016 /lib/libdl.so.0 -> libdl-0.9.33.2.so
-rw-r--r--    1 0        0            26604 Jan 31  2016 /lib/libfstools.so
-rw-r--r--    1 0        0            78648 Jan 31  2016 /lib/libgcc_s.so.1
-rw-r--r--    1 0        0             8500 Jan 31  2016 /lib/libjson_script.so
-rwxr-xr-x    1 0        0            89392 Jan 31  2016 /lib/libm-0.9.33.2.so
lrwxrwxrwx    1 0        0               16 Feb  2  2016 /lib/libm.so.0 -> libm-0.9.33.2.so
-rw-r--r--    1 0        0             2184 Jan 31  2016 /lib/libsetlbf.so
-rwxr-xr-x    1 0        0           359596 Jan 31  2016 /lib/libuClibc-0.9.33.2.so
-rw-r--r--    1 0        0            30200 Jan 31  2016 /lib/libubox.so
-rw-r--r--    1 0        0            14204 Jan 31  2016 /lib/libubus.so
-rwxr-xr-x    1 0        0            26512 Jan 31  2016 /lib/libuci.so
-rwxr-xr-x    1 0        0             4044 Jan 31  2016 /lib/libutil-0.9.33.2.so
lrwxrwxrwx    1 0        0               19 Feb  2  2016 /lib/libutil.so.0 -> libutil-0.9.33.2.so
-rw-r--r--    1 0        0            12004 Jan 31  2016 /lib/libvalidate.so
lrwxrwxrwx    1 0        0               17 Feb  2  2016 /usr/lib/libip4tc.so -> libip4tc.so.0.1.0
lrwxrwxrwx    1 0        0               17 Feb  2  2016 /usr/lib/libip4tc.so.0 -> libip4tc.so.0.1.0
-rwxr-xr-x    1 0        0            16992 Jan 31  2016 /usr/lib/libip4tc.so.0.1.0
lrwxrwxrwx    1 0        0               17 Feb  2  2016 /usr/lib/libip6tc.so -> libip6tc.so.0.1.0
lrwxrwxrwx    1 0        0               17 Feb  2  2016 /usr/lib/libip6tc.so.0 -> libip6tc.so.0.1.0
-rwxr-xr-x    1 0        0            17784 Jan 31  2016 /usr/lib/libip6tc.so.0.1.0
-rwxr-xr-x    1 0        0            51800 Jan 31  2016 /usr/lib/libiptext.so
-rwxr-xr-x    1 0        0            18852 Jan 31  2016 /usr/lib/libiptext4.so
-rwxr-xr-x    1 0        0            18684 Jan 31  2016 /usr/lib/libiptext6.so
-rwxr-xr-x    1 0        0            43308 Jan 31  2016 /usr/lib/libiwinfo.so
lrwxrwxrwx    1 0        0               18 Feb  2  2016 /usr/lib/libjson-c.so.2 -> libjson-c.so.2.0.1
-rwxr-xr-x    1 0        0            27012 Jan 31  2016 /usr/lib/libjson-c.so.2.0.1
-rw-r--r--    1 0        0           126336 Jan 31  2016 /usr/lib/liblua.so.5.1.5
-rwxr-xr-x    1 0        0            22980 Jan 31  2016 /usr/lib/libnl-tiny.so
lrwxrwxrwx    1 0        0               20 Feb  2  2016 /usr/lib/libxtables.so -> libxtables.so.10.0.0
lrwxrwxrwx    1 0        0               20 Feb  2  2016 /usr/lib/libxtables.so.10 -> libxtables.so.10.0.0
-rwxr-xr-x    1 0        0            29832 Jan 31  2016 /usr/lib/libxtables.so.10.0.0
-rwxr-xr-x    1 0        0            17036 Jan 31  2016 /usr/lib/lua/iwinfo.so
-rwxr-xr-x    1 0        0            17560 Jan 31  2016 /usr/lib/lua/luci/ip.so
-rwxr-xr-x    1 0        0            12803 Jan 31  2016 /usr/lib/lua/luci/template/parser.so
-rwxr-xr-x    1 0        0            63580 Jan 31  2016 /usr/lib/lua/nixio.so
-rwxr-xr-x    1 0        0            11908 Jan 31  2016 /usr/lib/lua/ubus.so
-rwxr-xr-x    1 0        0            12784 Jan 31  2016 /usr/lib/lua/uci.so
-rwxr-xr-x    1 0        0            17136 Jan 31  2016 /usr/lib/pppd/2.4.7/rp-pppoe.so
-rwxr-xr-x    1 0        0            11020 Jan 31  2016 /usr/lib/uhttpd_ubus.so
=== Static archives and headers ===
```

## Language runtimes and tools

```text
=== Command availability ===
sh: /bin/sh
ash: /bin/ash
lua: /usr/bin/lua
awk: /usr/bin/awk
sed: /bin/sed
grep: /bin/grep
expr: /usr/bin/expr
wget: /usr/bin/wget
nc: /usr/bin/nc
telnet: /usr/bin/telnet
ssh: /usr/bin/ssh
scp: /usr/bin/scp
telnetd: /usr/sbin/telnetd
uhttpd: /usr/sbin/uhttpd
dnsmasq: /usr/sbin/dnsmasq
dropbear: /usr/sbin/dropbear
ntpd: /usr/sbin/ntpd
hostapd: /usr/sbin/hostapd
wpa_supplicant: /usr/sbin/wpa_supplicant
rpcd: /sbin/rpcd
ubus: /bin/ubus
ubusd: /sbin/ubusd
procd: /sbin/procd
opkg: /bin/opkg
=== Version info ===
BusyBox v1.23.2 (2016-01-02 18:01:44 CET) multi-call binary.
BusyBox is copyrighted by many authors between 1998-2012.
Lua 5.1.5  Copyright (C) 1994-2012 Lua.org, PUC-Rio (double int32)
=== Lua package paths ===
package.path=./?.lua;/usr/share/lua/?.lua;/usr/share/lua/?/init.lua;/usr/lib/lua/?.lua;/usr/lib/lua/?/init.lua
package.cpath=./?.so;/usr/lib/lua/?.so;/usr/lib/lua/loadall.so
```

## Lua modules

```text
=== Lua shared objects and scripts ===
/usr/lib/lua/iwinfo.so
/usr/lib/lua/luci/cacheloader.lua
/usr/lib/lua/luci/cbi.lua
/usr/lib/lua/luci/cbi/datatypes.lua
/usr/lib/lua/luci/ccache.lua
/usr/lib/lua/luci/config.lua
/usr/lib/lua/luci/controller/admin/filebrowser.lua
/usr/lib/lua/luci/controller/admin/index.lua
/usr/lib/lua/luci/controller/admin/network.lua
/usr/lib/lua/luci/controller/admin/servicectl.lua
/usr/lib/lua/luci/controller/admin/status.lua
/usr/lib/lua/luci/controller/admin/system.lua
/usr/lib/lua/luci/controller/admin/uci.lua
/usr/lib/lua/luci/controller/firewall.lua
/usr/lib/lua/luci/controller/wol.lua
/usr/lib/lua/luci/debug.lua
/usr/lib/lua/luci/dispatcher.lua
/usr/lib/lua/luci/http.lua
/usr/lib/lua/luci/http/protocol.lua
/usr/lib/lua/luci/http/protocol/conditionals.lua
/usr/lib/lua/luci/http/protocol/date.lua
/usr/lib/lua/luci/http/protocol/mime.lua
/usr/lib/lua/luci/i18n.lua
/usr/lib/lua/luci/ip.so
/usr/lib/lua/luci/ltn12.lua
/usr/lib/lua/luci/model/cbi/admin_network/dhcp.lua
/usr/lib/lua/luci/model/cbi/admin_network/hosts.lua
/usr/lib/lua/luci/model/cbi/admin_network/iface_add.lua
/usr/lib/lua/luci/model/cbi/admin_network/ifaces.lua
/usr/lib/lua/luci/model/cbi/admin_network/network.lua
/usr/lib/lua/luci/model/cbi/admin_network/proto_6in4.lua
/usr/lib/lua/luci/model/cbi/admin_network/proto_6rd.lua
/usr/lib/lua/luci/model/cbi/admin_network/proto_6to4.lua
/usr/lib/lua/luci/model/cbi/admin_network/proto_ahcp.lua
/usr/lib/lua/luci/model/cbi/admin_network/proto_aiccu.lua
/usr/lib/lua/luci/model/cbi/admin_network/proto_dhcp.lua
/usr/lib/lua/luci/model/cbi/admin_network/proto_dhcpv6.lua
/usr/lib/lua/luci/model/cbi/admin_network/proto_dslite.lua
/usr/lib/lua/luci/model/cbi/admin_network/proto_hnet.lua
/usr/lib/lua/luci/model/cbi/admin_network/proto_l2tp.lua
/usr/lib/lua/luci/model/cbi/admin_network/proto_none.lua
/usr/lib/lua/luci/model/cbi/admin_network/proto_ppp.lua
/usr/lib/lua/luci/model/cbi/admin_network/proto_pppoa.lua
/usr/lib/lua/luci/model/cbi/admin_network/proto_pppoe.lua
/usr/lib/lua/luci/model/cbi/admin_network/proto_pptp.lua
/usr/lib/lua/luci/model/cbi/admin_network/proto_static.lua
/usr/lib/lua/luci/model/cbi/admin_network/routes.lua
/usr/lib/lua/luci/model/cbi/admin_network/vlan.lua
/usr/lib/lua/luci/model/cbi/admin_network/wifi.lua
/usr/lib/lua/luci/model/cbi/admin_network/wifi_add.lua
/usr/lib/lua/luci/model/cbi/admin_status/processes.lua
/usr/lib/lua/luci/model/cbi/admin_system/admin.lua
/usr/lib/lua/luci/model/cbi/admin_system/backupfiles.lua
/usr/lib/lua/luci/model/cbi/admin_system/buttons.lua
/usr/lib/lua/luci/model/cbi/admin_system/crontab.lua
/usr/lib/lua/luci/model/cbi/admin_system/fstab.lua
/usr/lib/lua/luci/model/cbi/admin_system/fstab/mount.lua
/usr/lib/lua/luci/model/cbi/admin_system/fstab/swap.lua
/usr/lib/lua/luci/model/cbi/admin_system/ipkg.lua
/usr/lib/lua/luci/model/cbi/admin_system/leds.lua
/usr/lib/lua/luci/model/cbi/admin_system/startup.lua
/usr/lib/lua/luci/model/cbi/admin_system/system.lua
/usr/lib/lua/luci/model/cbi/firewall/custom.lua
/usr/lib/lua/luci/model/cbi/firewall/forward-details.lua
/usr/lib/lua/luci/model/cbi/firewall/forwards.lua
/usr/lib/lua/luci/model/cbi/firewall/rule-details.lua
/usr/lib/lua/luci/model/cbi/firewall/rules.lua
/usr/lib/lua/luci/model/cbi/firewall/zone-details.lua
/usr/lib/lua/luci/model/cbi/firewall/zones.lua
/usr/lib/lua/luci/model/cbi/wol.lua
/usr/lib/lua/luci/model/firewall.lua
/usr/lib/lua/luci/model/ipkg.lua
/usr/lib/lua/luci/model/network.lua
/usr/lib/lua/luci/model/network/proto_4x6.lua
/usr/lib/lua/luci/model/network/proto_6x4.lua
/usr/lib/lua/luci/model/network/proto_aiccu.lua
/usr/lib/lua/luci/model/network/proto_dhcpv6.lua
/usr/lib/lua/luci/model/network/proto_hnet.lua
/usr/lib/lua/luci/model/network/proto_ppp.lua
/usr/lib/lua/luci/model/uci.lua
/usr/lib/lua/luci/sgi/cgi.lua
/usr/lib/lua/luci/sgi/uhttpd.lua
/usr/lib/lua/luci/store.lua
/usr/lib/lua/luci/sys.lua
/usr/lib/lua/luci/sys/iptparser.lua
/usr/lib/lua/luci/sys/zoneinfo.lua
/usr/lib/lua/luci/sys/zoneinfo/tzdata.lua
/usr/lib/lua/luci/sys/zoneinfo/tzoffset.lua
/usr/lib/lua/luci/template.lua
/usr/lib/lua/luci/template/parser.so
/usr/lib/lua/luci/tools/firewall.lua
/usr/lib/lua/luci/tools/proto.lua
/usr/lib/lua/luci/tools/status.lua
/usr/lib/lua/luci/tools/webadmin.lua
/usr/lib/lua/luci/util.lua
/usr/lib/lua/luci/version.lua
/usr/lib/lua/nixio.so
/usr/lib/lua/nixio/fs.lua
/usr/lib/lua/nixio/util.lua
/usr/lib/lua/ubus.so
/usr/lib/lua/uci.so
=== Lua module symlinks ===
```

### Additional Lua checks

`require()` succeeded for `nixio`, `nixio.fs`, `nixio.util`, `uci`, `ubus`, `iwinfo`, `luci.http`, `luci.model.uci`, `luci.ip`, `luci.ltn12`, and `luci.template.parser`. It did not succeed for `socket`, `ssl`, `lfs`, `cjson`, `luci.json`, or `luci.jsonc`. The checked `nixio` fields `socket`, `connect`, `bind`, `poll`, `gettimeofday`, `fork`, and `exec` were functions; `crypto` and `tls` were nil. `/usr/share/lua`, `/usr/local/share/lua`, and `/usr/local/lib/lua` were absent. `/usr/share/libubox/jshn.sh`, `/usr/bin/jshn`, and `/usr/bin/jsonfilter` existed; a JSON-filter test returned the expected value.

## BusyBox applets

```text
[
[[
arping
ash
awk
basename
brctl
bunzip2
bzcat
cat
chgrp
chmod
chown
chroot
clear
cmp
cp
crond
crontab
cut
date
dd
devmem
df
dirname
dmesg
du
echo
egrep
env
expr
false
fgrep
find
free
fsync
grep
gunzip
gzip
halt
head
hexdump
hostid
hwclock
id
ifconfig
kill
killall
less
ln
lock
logger
ls
md5sum
mkdir
mkfifo
mknod
mkswap
mktemp
mount
mv
nc
netmsg
netstat
nice
nslookup
ntpd
passwd
pgrep
pidof
ping
ping6
pivot_root
poweroff
printf
ps
pwd
readlink
reboot
reset
rm
rmdir
route
sed
seq
sh
sleep
sort
start-stop-daemon
strings
switch_root
sync
sysctl
tail
tar
tee
telnet
telnetd
test
time
top
touch
tr
traceroute
true
udhcpc
umount
uname
uniq
uptime
vconfig
vi
wc
wget
which
xargs
yes
zcat
```

## Network servers and IPC

```text
=== Listening TCP/UDP sockets ===
Active Internet connections (only servers)
Proto Recv-Q Send-Q Local Address           Foreign Address         State       PID/Program name    
tcp        0      0 0.0.0.0:80              0.0.0.0:*               LISTEN      949/uhttpd
tcp        0      0 0.0.0.0:53              0.0.0.0:*               LISTEN      1017/dnsmasq
tcp        0      0 192.168.0.254:22        0.0.0.0:*               LISTEN      867/dropbear
tcp        0      0 :::80                   :::*                    LISTEN      949/uhttpd
tcp        0      0 :::53                   :::*                    LISTEN      1017/dnsmasq
udp        0      0 0.0.0.0:53              0.0.0.0:*                           1017/dnsmasq
udp        0      0 :::53                   :::*                                1017/dnsmasq
=== Process names, not arguments ===
PID USER COMMAND
1 root /sbin/procd
2 root [kthreadd]
3 root [ksoftirqd/0]
5 root [kworker/0:0H]
6 root [kworker/u2:0]
7 root [khelper]
27 root [writeback]
29 root [bioset]
30 root [kblockd]
31 root [kswapd0]
32 root [kworker/0:1]
99 root [fsnotify_mark]
149 root [spi0]
265 root [deferwq]
266 root [kworker/u2:2]
316 root [kworker/0:2]
346 root [jffs2_gcd_mtd3]
430 root /sbin/ubusd
431 root /sbin/askfirst
561 root [ipv6_addrconf]
616 root [cfg80211]
722 root /sbin/logd
731 root /sbin/rpcd
765 root /sbin/netifd
789 root /usr/sbin/odhcpd
867 root /usr/sbin/dropbear
949 root /usr/sbin/uhttpd
1017 nobody /usr/sbin/dnsmasq
1065 root /usr/sbin/ntpd
1115 root /usr/sbin/hostapd
16977 root /usr/sbin/dropbear
16978 root /usr/sbin/dropbear
16979 root ash
16981 root ps
16982 root awkNR
=== Available ubus objects ===
dhcp
hostapd.wlan0
log
network
network.device
network.interface
network.interface.lan
network.interface.loopback
network.interface.wan
network.wireless
service
session
system
uci
=== CGI executables ===
/www/cgi-bin/luci
=== Standalone executable directories ===
--- /bin ---
ash
board_detect
busybox
cat
chgrp
chmod
chown
config_generate
cp
date
dd
df
dmesg
echo
egrep
false
fgrep
fsync
grep
gunzip
gzip
ipcalc.sh
kill
ln
lock
login.sh
ls
mkdir
mknod
mktemp
mount
mv
netmsg
netstat
nice
opkg
pidof
ping
ping6
ps
pwd
rm
rmdir
sed
sh
sleep
sync
tar
touch
true
ubus
umount
uname
vi
zcat
--- /sbin ---
askfirst
devmem
devstatus
firstboot
fw3
halt
hotplug-call
hwclock
ifconfig
ifdown
ifstatus
ifup
init
jffs2mark
jffs2reset
kmodloader
led.sh
logd
logread
luci-reload
mkswap
mount_root
mtd
netifd
pivot_root
poweroff
procd
reboot
reload_config
route
rpcd
snapshot
snapshot_tool
start-stop-daemon
swconfig
switch_root
sysctl
sysupgrade
ubusd
uci
udevtrigger
udhcpc
validate_data
vconfig
wget2nand
wifi
--- /usr/bin ---
[
[[
awk
basename
bunzip2
bzcat
clear
cmp
crontab
cut
dbclient
dirname
dropbearkey
du
env
etherwake
expr
find
free
head
hexdump
hostid
id
jshn
jsonfilter
killall
less
logger
lua
luci-bwc
md5sum
mkfifo
nc
nslookup
passwd
pgrep
printf
readlink
reset
scp
seq
signify
sort
ssh
strings
tail
tee
telnet
test
time
top
tr
traceroute
uniq
uptime
usign
wc
wget
which
xargs
yes
--- /usr/sbin ---
arping
brctl
chroot
crond
dnsmasq
dropbear
fw_printenv
fw_setenv
hostapd
insmod
ip6tables
ip6tables-restore
ip6tables-save
iptables
iptables-restore
iptables-save
iw
lsmod
modinfo
modprobe
ntpd
ntpd-hotplug
odhcp6c
odhcpd
odhcpd-update
opkg-key
pppd
rmmod
telnetd
uhttpd
wpa_supplicant
wpad
xtables-multi
```

## Installed packages

```text
base-files - 157.2-r48532
busybox - 1.23.2-1
dnsmasq - 2.73-1
dropbear - 2015.67-1
etherwake - 1.09-3
firewall - 2015-07-27
fstools - 2016-01-10-96415afecef35766332067f4205ef3b2c7561d21
hostapd-common - 2015-03-25-1
ip6tables - 1.4.21-1
iptables - 1.4.21-1
iw - 3.17-1
jshn - 2015-11-08-10429bccd0dc5d204635e110a7a8fae7b80d16cb
jsonfilter - 2014-06-19-cdc760c58077f44fc40adbbe41e1556a67c1b9a9
kernel - 3.18.23-1-b2f200610f46d20ef52d269421369d0c
kmod-ath - 3.18.23+2015-03-09-3
kmod-ath9k - 3.18.23+2015-03-09-3
kmod-ath9k-common - 3.18.23+2015-03-09-3
kmod-cfg80211 - 3.18.23+2015-03-09-3
kmod-crypto-aes - 3.18.23-1
kmod-crypto-arc4 - 3.18.23-1
kmod-crypto-core - 3.18.23-1
kmod-gpio-button-hotplug - 3.18.23-1
kmod-ip6tables - 3.18.23-1
kmod-ipt-conntrack - 3.18.23-1
kmod-ipt-core - 3.18.23-1
kmod-ipt-nat - 3.18.23-1
kmod-ipv6 - 3.18.23-1
kmod-lib-crc-ccitt - 3.18.23-1
kmod-mac80211 - 3.18.23+2015-03-09-3
kmod-nf-conntrack - 3.18.23-1
kmod-nf-conntrack6 - 3.18.23-1
kmod-nf-ipt - 3.18.23-1
kmod-nf-ipt6 - 3.18.23-1
kmod-nf-nat - 3.18.23-1
kmod-nf-nathelper - 3.18.23-1
kmod-ppp - 3.18.23-1
kmod-pppoe - 3.18.23-1
kmod-pppox - 3.18.23-1
kmod-slhc - 3.18.23-1
libblobmsg-json - 2015-11-08-10429bccd0dc5d204635e110a7a8fae7b80d16cb
libc - 0.9.33.2-1
libgcc - 4.8-linaro-1
libip4tc - 1.4.21-1
libip6tc - 1.4.21-1
libiwinfo - 2015-06-01-ade8b1b299cbd5748db1acf80dd3e9f567938371
libiwinfo-lua - 2015-06-01-ade8b1b299cbd5748db1acf80dd3e9f567938371
libjson-c - 0.12-1
libjson-script - 2015-11-08-10429bccd0dc5d204635e110a7a8fae7b80d16cb
liblua - 5.1.5-1
libnl-tiny - 0.1-4
libubox - 2015-11-08-10429bccd0dc5d204635e110a7a8fae7b80d16cb
libubus - 2015-05-25-f361bfa5fcb2daadf3b160583ce665024f8d108e
libubus-lua - 2015-05-25-f361bfa5fcb2daadf3b160583ce665024f8d108e
libuci - 2015-08-27.1-1
libuci-lua - 2015-08-27.1-1
libxtables - 1.4.21-1
lua - 5.1.5-1
luci - git-16.018.33482-3201903-1
luci-app-firewall - git-16.018.33482-3201903-1
luci-app-wol - git-16.018.33482-3201903-1
luci-base - git-16.018.33482-3201903-1
luci-lib-ip - git-16.018.33482-3201903-1
luci-lib-nixio - git-16.018.33482-3201903-1
luci-mod-admin-full - git-16.018.33482-3201903-1
luci-proto-ipv6 - git-16.018.33482-3201903-1
luci-proto-ppp - git-16.018.33482-3201903-1
luci-theme-bootstrap - git-16.018.33482-3201903-1
mtd - 21
netifd - 2015-12-16-245527193e90906451be35c2b8e972b8712ea6ab
odhcp6c - 2015-07-13-024525798c5f6aba3af9b2ef7b3af2f3c14f1db8
odhcpd - 2015-11-19-01d3f9d64486ac1daa144848944e877e7f0cb762
opkg - 9c97d5ecd795709c8584e972bfdf3aee3a5b846d-9
ppp - 2.4.7-6
ppp-mod-pppoe - 2.4.7-6
procd - 2015-10-29.1-d5fddd91b966424bb63e943e789704d52382cc18
rpcd - 2015-01-10-f00890cd6eb47ad9bb5da0fb6c50aedc8406e7c5
swconfig - 10
uboot-envtools - 2014.10-2
ubox - 2015-11-22-c086167a0154745c677f8730a336ea9cf7d71031
ubus - 2015-05-25-f361bfa5fcb2daadf3b160583ce665024f8d108e
ubusd - 2015-05-25-f361bfa5fcb2daadf3b160583ce665024f8d108e
uci - 2015-08-27.1-1
uhttpd - 2015-11-08-fe01ef3f52adae9da38ef47926cd50974af5d6b7
uhttpd-mod-ubus - 2015-11-08-fe01ef3f52adae9da38ef47926cd50974af5d6b7
usign - 2015-05-08-cf8dcdb8a4e874c77f3e9a8e9b643e8c17b19131
wpad-mini - 2015-03-25-1
```
