#!/bin/sh
# keyboard layouts (data only) for libxkbcommon: /usr/share/X11/xkb
. "$(dirname "$0")/common.sh"
V=2.43
T=$(fetch https://www.x.org/releases/individual/data/xkeyboard-config/xkeyboard-config-$V.tar.xz)
S=$PORTS_SRC/xkeyboard-config-$V; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/xkeyboard-config" -Dxkb-base=/usr/share/X11/xkb -Dcompat-rules=true -Dxorg-rules-symlinks=false
echo "xkeyboard-config $V installed"
