#!/bin/sh
. "$(dirname "$0")/common.sh"
G=$PORTS_BUILD/wlgltest; mkdir -p "$G"
XML=$PREFIX_ROOT/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
SCANNER=${HOST_TOOLS:-${TOOLS_DIR:-/data/tools}/host}/bin/wayland-scanner
"$SCANNER" client-header "$XML" "$G/xdg-shell-client-protocol.h"
"$SCANNER" private-code "$XML" "$G/xdg-shell-protocol.c"
"$UL/musl-dcc" -std=c11 -O2 -I"$G" -I"$PREFIX_ROOT/usr/include" \
    "$UL/ports/src/wlgltest.c" "$G/xdg-shell-protocol.c" \
    -L"$PREFIX_ROOT/usr/lib" -Wl,-rpath-link,"$PREFIX_ROOT/usr/lib" \
    -lEGL -lGLESv2 -lwayland-egl -lwayland-client -o "$PREFIX_ROOT/usr/bin/wlgltest"
cp "$UL/ports/scripts/glsmoke" "$PREFIX_ROOT/usr/bin/glsmoke"
echo "wlgltest + glsmoke built"
