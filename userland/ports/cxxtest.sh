#!/bin/sh
. "$(dirname "$0")/common.sh"
"$UL/musl-dcxx" -std=c++17 -O2 -fexceptions -frtti \
    "$UL/ports/src/cxxtest.cc" -o "$PREFIX_ROOT/usr/bin/cxxtest"
# Retain the M38/Mesa no-EH/no-RTTI consumer configuration against the full runtime.
"$UL/musl-dcxx" -std=c++17 -O2 -fno-exceptions -fno-rtti -DCXXTEST_NO_EH \
    "$UL/ports/src/cxxtest.cc" -o "$PREFIX_ROOT/usr/bin/cxxtest-noeh"
echo "cxxtest built"
