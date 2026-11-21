#!/bin/sh
# Builds the openwrt-wr841n firmware for the TP-Link TL-WR841N/ND v9 with the
# OpenWrt 15.05.1 Image Builder: no LuCI/Lua, no IPv6, no PPP, no dnsmasq; with the
# admin page, huawei-trafficd, lanwatch, tinymdns, weatherd and tvremote built in.
#   firmware/build.sh [IMAGEBUILDER_DIR]
# Output: firmware/bin/ (git-ignored). Nothing is flashed.
set -e
cd "$(dirname "$0")/.."
TOP=$PWD
IB=${1:-$TOP/toolchain/OpenWrt-ImageBuilder-15.05.1-ar71xx-generic.Linux-x86_64}
STAGE=$TOP/firmware/.stage
OUT=$TOP/firmware/bin
[ -d "$IB" ] || { echo "Image Builder not found: $IB" >&2; exit 1; }

# Packages: the default set minus what this access point doesn't use, plus what our programs need.
PACKAGES="-dnsmasq -ip6tables -ppp -ppp-mod-pppoe -odhcpd -odhcp6c
	iw uhttpd uhttpd-mod-ubus rpcd rpcd-mod-file rpcd-mod-iwinfo rpcd-mod-rpcsys cgi-io libiwinfo"

# Files baked into the read-only part of the image.
rm -rf "$STAGE"; mkdir -p "$STAGE"
cp -a firmware/files/. "$STAGE/"
inst() { mode=$1 src=$2 dst=$STAGE$3; mkdir -p "$(dirname "$dst")"; cp "$src" "$dst"; chmod "$mode" "$dst"; }
for d in huawei-trafficd lanwatch tinymdns weatherd tvremote; do
	inst 755 $d/$d /usr/sbin/$d
	inst 755 $d/files/$d.init /etc/init.d/$d
	inst 644 $d/files/$d.config /etc/config/$d
	mkdir -p "$STAGE/etc/rc.d"	# enabled at boot, like "/etc/init.d/$d enable" (START=95, STOP=10)
	ln -s ../init.d/$d "$STAGE/etc/rc.d/S95$d"
	ln -s ../init.d/$d "$STAGE/etc/rc.d/K10$d"
done
inst 644 huawei-trafficd/files/huawei-trafficd.acl.json /usr/share/rpcd/acl.d/huawei-trafficd.json
inst 644 lanwatch/files/lanwatch.acl.json /usr/share/rpcd/acl.d/lanwatch.json
inst 644 weatherd/files/weatherd.acl.json /usr/share/rpcd/acl.d/weatherd.json
inst 644 tvremote/files/tvremote.acl.json /usr/share/rpcd/acl.d/tvremote.json
inst 644 lanwatch/files/oui.bin /usr/share/lanwatch/oui.bin
inst 644 huawei-trafficd/www/traffic.html /www/traffic.html
inst 644 lanwatch/www/devices.html /www/devices.html
inst 644 weatherd/www/weather.html /www/weather.html
inst 644 tvremote/www/remote.html /www/remote.html
inst 644 www/index.html /www/index.html
inst 644 www/favicon.svg /www/favicon.svg
inst 644 www/favicon-32.png /www/favicon-32.png
inst 644 www/apple-touch-icon.png /www/apple-touch-icon.png
inst 755 www/cgi-bin/routerinfo /www/cgi-bin/routerinfo
inst 755 gemgate/gemgate /www/cgi-bin/gemini
inst 644 gemgate/www/gemini.html /www/gemini.html
inst 644 admin/www/admin.html /www/admin.html
inst 755 admin/files/admin-apply /usr/libexec/admin-apply
inst 644 admin/files/admin.acl.json /usr/share/rpcd/acl.d/admin.json

# Package indexes from the archive (downloads.openwrt.org no longer serves 15.05.1).
R=https://archive.openwrt.org/chaos_calmer/15.05.1/ar71xx/generic/packages
cat > "$IB/repositories.conf" <<EOR
src/gz chaos_calmer_base $R/base
src/gz chaos_calmer_packages $R/packages
src imagebuilder file:packages
EOR

mkdir -p "$OUT"
make -C "$IB" image PROFILE=TLWR841 PACKAGES="$(echo $PACKAGES)" FILES="$STAGE" BIN_DIR="$OUT"
ls -l "$OUT"/*tl-wr841n-v9*
