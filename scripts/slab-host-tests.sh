#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
CC=${CC:-clang}
mkdir -p build/slab-host
"$CC" -std=c2x -O1 -g -ffunction-sections -fdata-sections \
    -fsanitize=undefined -fsanitize-trap=all -fno-omit-frame-pointer \
    -Wall -Wextra -Wno-unused-parameter \
    -Itests/slab-host -Ikernel/include -Ikernel/arch/x86_64/include -Ithird_party/limine \
    tests/slab-host/objects.c -pthread -Wl,--gc-sections -o build/slab-host/objects
build/slab-host/objects
