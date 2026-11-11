#!/bin/sh
# Builds and runs the host-side tests (needs a C compiler and libcurl; uses the network).
set -e
cd "$(dirname "$0")/.."
mkdir -p tests/out
cc -std=gnu11 -Wall -Wextra -g -fsanitize=address,undefined -DAPP_DIR='"tests/out"' \
   tests/host_test.c source/util.c source/feed.c source/net.c source/stream.c source/plat.c \
   -lcurl -lpthread -o tests/out/host_test
./tests/out/host_test "$@"
