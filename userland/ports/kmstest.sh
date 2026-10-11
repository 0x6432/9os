#!/bin/sh
# kmstest: DRM atomic/PRIME regression test (needs the libdrm port)
. "$(dirname "$0")/common.sh"
$DCC -O2 -o "$PREFIX_ROOT/usr/bin/kmstest" "$(dirname "$0")/src/kmstest.c" -I"$PREFIX_ROOT/usr/include" \
    -I"$PREFIX_ROOT/usr/include/libdrm" -L"$PREFIX_ROOT/usr/lib" -Wl,-rpath-link,"$PREFIX_ROOT/usr/lib" -ldrm
echo "kmstest built"
