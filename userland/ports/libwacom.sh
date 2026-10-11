#!/bin/sh
# libwacom: tablet descriptions for libinput (data in /usr/share/libwacom, udev hwdb/rules)
. "$(dirname "$0")/common.sh"
V=2.14.0
T=$(fetch https://github.com/linuxwacom/libwacom/releases/download/libwacom-$V/libwacom-$V.tar.xz)
S=$PORTS_SRC/libwacom-$V; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/libwacom" -Dtests=disabled -Ddocumentation=disabled -Dudev-dir=/usr/lib/udev
echo "libwacom $V installed"
