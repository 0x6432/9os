#!/bin/sh
. "$(dirname "$0")/common.sh"
V=1.7.0
T=$(fetch https://xkbcommon.org/download/libxkbcommon-$V.tar.xz)
S=$PORTS_SRC/libxkbcommon-$V; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/libxkbcommon" -Denable-x11=false -Denable-wayland=false -Denable-docs=false \
    -Denable-tools=false -Denable-xkbregistry=false -Dxkb-config-root=/usr/share/X11/xkb
echo "libxkbcommon $V installed"
