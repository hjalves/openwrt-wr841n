SDK ?= $(abspath ../toolchain/OpenWrt-SDK-15.05.1-ar71xx-generic_gcc-4.8-linaro_uClibc-0.9.33.2.Linux-x86_64)
STAGING_DIR := $(SDK)/staging_dir
TOOLCHAIN := $(STAGING_DIR)/toolchain-mips_34kc_gcc-4.8-linaro_uClibc-0.9.33.2
export STAGING_DIR

CC = $(TOOLCHAIN)/bin/mips-openwrt-linux-uclibc-gcc
AR = $(TOOLCHAIN)/bin/mips-openwrt-linux-uclibc-ar
BEARSSL = bearssl
BEARSSL_COMMIT = 7bea48e5e850ab4cafbe68d3765cdaba13a86d6f
ARCH_CFLAGS = -Os -mips32r2 -mtune=24kc -msoft-float -ffunction-sections -fdata-sections
CFLAGS = -std=gnu99 -Wall -Wextra $(ARCH_CFLAGS)
LDFLAGS = -s -Wl,--gc-sections -Wl,-z,noexecstack

.PHONY: all clean bearssl

all: gemgate

# BearSSL: fetched by ./fetch-bearssl.sh (git, pinned commit), built for MIPS here.
$(BEARSSL)/build-mips/libbearssl.a:
	@test -d $(BEARSSL) || { echo "run ./fetch-bearssl.sh first"; exit 1; }
	printf 'include conf/Unix.mk\nBUILD = build-mips\nCC = %s\nAR = %s\nCFLAGS = -W -Wall %s\n' \
		'$(CC)' '$(AR)' '$(ARCH_CFLAGS)' > $(BEARSSL)/conf/MipsOpenWrt.mk
	$(MAKE) -C $(BEARSSL) CONF=MipsOpenWrt lib

gemgate: gemgate.c $(BEARSSL)/build-mips/libbearssl.a Makefile
	"$(CC)" $(CFLAGS) -I$(BEARSSL)/inc $< -o $@ $(LDFLAGS) $(BEARSSL)/build-mips/libbearssl.a

clean:
	rm -f gemgate
