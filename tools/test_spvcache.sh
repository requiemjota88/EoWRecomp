#!/bin/sh
# Test of the persistent SPIR-V cache (runtime/nvn/vulkan/spvcache.c): corruption, truncation, environment change,
# legacy directories, validator veto, many threads. No GPU, no game files. Works on Linux, macOS and MSYS2.
# usage: tools/test_spvcache.sh
set -e
cd "$(dirname "$0")/.."
OUT=build/test_spvcache
mkdir -p "$OUT"
CC=${CC:-clang}
PTHREAD=""
case "$(uname -s)" in MINGW*|MSYS*) ;; *) PTHREAD="-lpthread";; esac
$CC -O1 -g -Wall -Wextra -Wno-unused-parameter tools/test_spvcache.c runtime/nvn/vulkan/spvcache.c -o "$OUT/test_spvcache" $PTHREAD
"$OUT/test_spvcache" "$OUT/scratch"
