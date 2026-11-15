SDK ?= $(abspath ../toolchain/OpenWrt-SDK-15.05.1-ar71xx-generic_gcc-4.8-linaro_uClibc-0.9.33.2.Linux-x86_64)
STAGING_DIR := $(SDK)/staging_dir
TARGET := $(STAGING_DIR)/target-mips_34kc_uClibc-0.9.33.2
TOOLCHAIN := $(STAGING_DIR)/toolchain-mips_34kc_gcc-4.8-linaro_uClibc-0.9.33.2
export STAGING_DIR

CC = $(TOOLCHAIN)/bin/mips-openwrt-linux-uclibc-gcc
CFLAGS = -std=gnu99 -Os -Wall -Wextra -Wpedantic -mips32r2 -mtune=24kc -msoft-float
CPPFLAGS = -isystem "$(TARGET)/usr/include"
LDFLAGS = -L"$(TARGET)/usr/lib" -Wl,-rpath-link,"$(TARGET)/usr/lib" -s -Wl,-z,noexecstack
LDLIBS = -lubus -lubox

.PHONY: all clean

all: huawei-trafficd

huawei-trafficd: huawei-trafficd.c Makefile
	"$(CC)" $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

clean:
	rm -f huawei-trafficd
