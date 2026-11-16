#!/bin/sh
# Builds OpenWrt 15.05's mksquashfs4/unsquashfs4 (squashfs-tools 4.2 + OpenWrt's patches)
# natively and puts them in the Image Builder. Needed on non-x86 build hosts: the Image
# Builder's own x86-64 mksquashfs4 crashes under qemu-user. Same source and patches, so
# the output format is the same. Needs a C compiler and liblzma (xz) headers.
#   firmware/native-squashfs.sh [IMAGEBUILDER_DIR]
set -e
cd "$(dirname "$0")/.."
IB=${1:-$PWD/toolchain/OpenWrt-ImageBuilder-15.05.1-ar71xx-generic.Linux-x86_64}
BIN=$IB/staging_dir/host/bin
W=$PWD/toolchain/squashfs4-native
P=https://raw.githubusercontent.com/openwrt/archive/chaos_calmer/tools/squashfs4/patches
PATCHES="100-portability 110-allow_static_liblzma 120-cygwin_fixes 150-freebsd_fixes
	160-expose_lzma_xz_options 170-add_support_for_LZMA_MAGIC_to_unsqashfs 180-openbsd_compat
	190-no_nonstatic_inline"

rm -rf "$W"; mkdir -p "$W"; cd "$W"
curl -sfL -o squashfs4.2.tar.gz https://sources.openwrt.org/squashfs4.2.tar.gz
echo "1b7a781fb4cf8938842279bd3e8ee852  squashfs4.2.tar.gz" | md5sum -c -	# OpenWrt's PKG_MD5SUM
tar xzf squashfs4.2.tar.gz
for p in $PATCHES; do curl -sfL "$P/$p.patch" | patch -d squashfs4.2 -p1 -s; done
# Old code, new compiler and glibc: major()/minor() moved to sys/sysmacros.h, and GCC 10+
# rejects some 2011-era C by default.
make -s -C squashfs4.2/squashfs-tools XZ_SUPPORT=1 LZMA_XZ_SUPPORT=1 XATTR_SUPPORT= LZMA_LIB=-llzma \
	EXTRA_CFLAGS="-include sys/sysmacros.h -fcommon -std=gnu89 -w" mksquashfs unsquashfs
for t in mksquashfs unsquashfs; do
	[ -L "$BIN/${t}4" ] && mv "$BIN/${t}4" "$BIN/${t}4.x86"	# keep the original
	cp squashfs4.2/squashfs-tools/$t "$BIN/${t}4"
done
"$BIN/mksquashfs4" -version | head -1
