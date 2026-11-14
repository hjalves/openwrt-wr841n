CC = mips-openwrt-linux-uclibc-gcc
CFLAGS = -Os -Wall -Wextra -Wpedantic -mips32r2 -mtune=24kc -msoft-float
LDFLAGS = -s -Wl,-z,noexecstack

.PHONY: all clean

all: hello-http

hello-http: hello-http.c Makefile
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f hello-http
