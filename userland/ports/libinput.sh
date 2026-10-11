#!/bin/sh
. "$(dirname "$0")/common.sh"
V=1.27.1
T=$(fetch https://gitlab.freedesktop.org/libinput/libinput/-/archive/$V/libinput-$V.tar.gz)
S=$PORTS_SRC/libinput-$V; [ -d "$S" ] || tar xzf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/libinput" -Dlibwacom=false -Ddebug-gui=false -Dtests=false -Ddocumentation=false \
    -Dudev-dir=/usr/lib/udev -Dinstall-tests=false -Dzshcompletiondir=no
echo "libinput $V installed"
