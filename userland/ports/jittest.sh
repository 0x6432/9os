#!/bin/sh
. "$(dirname "$0")/common.sh"
"$UL/musl-dcc" -std=c11 -O2 -I"$PREFIX_ROOT/usr/include" \
    "$UL/ports/src/jittest.c" -L"$PREFIX_ROOT/usr/lib" -Wl,-rpath-link,"$PREFIX_ROOT/usr/lib" \
    -lLLVM-18 -o "$PREFIX_ROOT/usr/bin/jittest"
echo "jittest built"
