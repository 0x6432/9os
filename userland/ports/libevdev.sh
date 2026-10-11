#!/bin/sh
. "$(dirname "$0")/common.sh"
V=1.13.3
T=$(fetch https://www.freedesktop.org/software/libevdev/libevdev-$V.tar.xz)
S=$PORTS_SRC/libevdev-$V; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/libevdev" -Dtests=disabled -Ddocumentation=disabled -Dcoverity=false
echo "libevdev $V installed"
