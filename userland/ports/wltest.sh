#!/bin/sh
# wltest: libwayland client/server smoke test (needs the wayland port)
. "$(dirname "$0")/common.sh"
$DCC -O2 -o "$PREFIX_ROOT/usr/bin/wltest" "$(dirname "$0")/src/wltest.c" -I"$PREFIX_ROOT/usr/include" \
    -L"$PREFIX_ROOT/usr/lib" -Wl,-rpath-link,"$PREFIX_ROOT/usr/lib" -lwayland-server -lwayland-client
echo "wltest built"
