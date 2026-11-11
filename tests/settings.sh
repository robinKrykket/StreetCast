#!/bin/sh
# Verifies settings persist between runs of the app.
set -e
cd "$(dirname "$0")/.."
rm -rf tests/out/settings && mkdir -p tests/out/settings
cc -std=gnu11 -Wall -g -fsanitize=address,undefined -DAPP_DIR='"tests/out/settings"' \
   tests/settings_test.c source/store.c source/util.c -o tests/out/settings_test
./tests/out/settings_test write
./tests/out/settings_test read
