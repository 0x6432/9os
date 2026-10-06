#!/bin/sh
# Host test of the kernel red-black tree (kernel/lib/rbtree.c) with UBSan traps.
set -eu
cd "$(dirname "$0")/.."
CC=${CC:-clang}
mkdir -p build/rbtree-host
"$CC" -std=c2x -O1 -g -fsanitize=undefined -fsanitize-trap=all -Wall -Wextra -Itests/rbtree-host \
    tests/rbtree-host/rbtest.c -o build/rbtree-host/rbtest
build/rbtree-host/rbtest
