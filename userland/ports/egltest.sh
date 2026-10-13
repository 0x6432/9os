#!/bin/sh
. "$(dirname "$0")/common.sh"
"$UL/musl-dcc" -std=c11 -O2 -I"$PREFIX_ROOT/usr/include" \
    "$UL/ports/src/egltest.c" -L"$PREFIX_ROOT/usr/lib" -Wl,-rpath-link,"$PREFIX_ROOT/usr/lib" \
    -lEGL -lGLESv2 -o "$PREFIX_ROOT/usr/bin/egltest"
echo "egltest built"
