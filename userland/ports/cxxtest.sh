#!/bin/sh
. "$(dirname "$0")/common.sh"
"$UL/musl-dcxx" -std=c++17 -O2 -fno-exceptions -fno-rtti \
    "$UL/ports/src/cxxtest.cc" -o "$PREFIX_ROOT/usr/bin/cxxtest"
echo "cxxtest built"
