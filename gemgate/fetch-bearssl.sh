#!/bin/sh
# Fetch BearSSL into gemgate/bearssl (git-ignored) at the pinned commit:
# the main branch of April 2026, which has security fixes made after the
# last release (v0.6, 2018).
set -e
cd "$(dirname "$0")"
COMMIT=7bea48e5e850ab4cafbe68d3765cdaba13a86d6f
[ -d bearssl ] || git clone https://www.bearssl.org/git/BearSSL bearssl
git -C bearssl fetch -q origin
git -C bearssl checkout -q "$COMMIT"
echo "BearSSL at $(git -C bearssl rev-parse HEAD)"
