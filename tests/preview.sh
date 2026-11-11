#!/bin/sh
# macOS only: renders StreetCast's screens to tests/out/shots/*.png using the real app logic.
set -e
cd "$(dirname "$0")/.."
mkdir -p tests/out
cc -fobjc-arc -g -fsanitize=address,undefined -Wall -DAPP_DIR='"tests/out/preview"' \
   tests/preview.m source/app.c source/ui.c source/util.c source/feed.c source/net.c source/store.c source/dl.c source/plat.c \
   -framework Cocoa -framework CoreText -lcurl -lpthread -o tests/out/preview_bin
./tests/out/preview_bin "$@"
