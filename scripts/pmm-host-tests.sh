#!/bin/sh
# Deterministic and concurrent tests of the real buddy/per-CPU page allocator.
set -eu
cd "$(dirname "$0")/.."
CC=${CC:-clang}
mkdir -p build/pmm-host
"$CC" -std=c2x -O1 -g -ffunction-sections -fdata-sections \
    -fsanitize=undefined -fsanitize-trap=all -fno-omit-frame-pointer \
    -Wall -Wextra -Wno-unused-parameter \
    -Itests/pmm-host -Ikernel/include -Ikernel/arch/x86_64/include -Ithird_party/limine \
    tests/pmm-host/pages.c -pthread -Wl,--gc-sections -o build/pmm-host/pages
build/pmm-host/pages