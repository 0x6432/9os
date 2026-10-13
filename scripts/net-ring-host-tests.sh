#!/bin/sh
# Real socket-ring tests with fragmented memory and allocation-failure injection.
set -eu
cd "$(dirname "$0")/.."
CC=${CC:-clang}
mkdir -p build/net-ring-host
"$CC" -std=c2x -O1 -g -fsanitize=undefined -fsanitize-trap=all -fno-omit-frame-pointer \
    -Wall -Wextra -Itests/net-ring-host -Ikernel/include \
    tests/net-ring-host/ringtest.c -o build/net-ring-host/ringtest
build/net-ring-host/ringtest
