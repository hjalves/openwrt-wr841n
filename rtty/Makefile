# rtty 9.2.1 (no TLS) for the TL-WR841N v9 / OpenWrt 15.05.1 (MIPS32r2, uClibc).
#
#   make fetch   download the pinned sources into src/ (git-ignored) and verify them
#   make         build ./rtty
#
# The downloaded sources are only compiled; their own build scripts (libev's
# configure, rtty's CMake) are never run. ev-config.h and config.h replace them.

SDK ?= $(abspath ../toolchain/OpenWrt-SDK-15.05.1-ar71xx-generic_gcc-4.8-linaro_uClibc-0.9.33.2.Linux-x86_64)
STAGING_DIR := $(SDK)/staging_dir
TOOLCHAIN := $(STAGING_DIR)/toolchain-mips_34kc_gcc-4.8-linaro_uClibc-0.9.33.2
export STAGING_DIR
CC = $(TOOLCHAIN)/bin/mips-openwrt-linux-uclibc-gcc

RTTY_COMMIT   = b0429e7cb3531a143993af31f9da16ff16492409  # v9.2.1 + 3 commits (2026-11-07)
LIBEV_URL     = http://dist.schmorp.de/libev/Attic/libev-4.33.tar.gz
LIBEV_SHA256  = 507eb7b8d1015fbec5b935f34ebed15bf346bed04a11ab82b8eee848c4205aea
INIH_URL      = https://github.com/benhoyt/inih/archive/refs/tags/r58.tar.gz
INIH_SHA256   = e79216260d5dffe809bda840be48ab0eec7737b2bb9f02d2275c1b46344ea7b7

CFLAGS = -Os -mips32r2 -mtune=24kc -msoft-float -std=gnu99 -D_GNU_SOURCE \
         -ffunction-sections -fdata-sections
# uClibc 0.9.33's headers lack O_PATH; this is the value in Linux 3.18's
# asm-generic/fcntl.h, which MIPS uses.
RTTY_DEFS = -DO_PATH=010000000
LDFLAGS = -Wl,--gc-sections -s -Wl,-z,noexecstack
LDLIBS = -lutil -lm

RTTY_SRC = $(wildcard src/rtty/src/*.c src/rtty/src/log/*.c src/rtty/src/buffer/*.c)

.PHONY: all fetch clean distclean

all: rtty

rtty: config.h ev-config.h src/ev.o src/ini.o $(RTTY_SRC) Makefile
	@test -n "$(RTTY_SRC)" || { echo "run 'make fetch' first" >&2; exit 1; }
	"$(CC)" $(CFLAGS) $(RTTY_DEFS) -I. -Isrc/rtty/src -Isrc/rtty/src/buffer -Isrc/libev -Isrc/inih \
		$(RTTY_SRC) src/ev.o src/ini.o -o $@ $(LDFLAGS) $(LDLIBS)

src/ev.o: ev-config.h src/libev/ev.c
	"$(CC)" $(CFLAGS) -w -DEV_CONFIG_H='"ev-config.h"' -I. -Isrc/libev -c src/libev/ev.c -o $@

src/ini.o: src/inih/ini.c
	"$(CC)" $(CFLAGS) -w -c $< -o $@

fetch:
	mkdir -p src
	curl -sSfL -o src/libev.tar.gz $(LIBEV_URL)
	echo "$(LIBEV_SHA256)  src/libev.tar.gz" | sha256sum -c
	curl -sSfL -o src/inih.tar.gz $(INIH_URL)
	echo "$(INIH_SHA256)  src/inih.tar.gz" | sha256sum -c
	rm -rf src/libev src/inih src/rtty
	mkdir -p src/libev src/inih
	tar xzf src/libev.tar.gz -C src/libev --strip-components=1
	tar xzf src/inih.tar.gz -C src/inih --strip-components=1
	git clone -q https://github.com/zhaojh329/rtty.git src/rtty
	git -C src/rtty checkout -q $(RTTY_COMMIT)
	git -C src/rtty submodule update -q --init src/buffer src/log

clean:
	rm -f src/ev.o src/ini.o

distclean:
	rm -rf src
