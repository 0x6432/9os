#!/bin/sh
. "$(dirname "$0")/common.sh"
V=0.44.2
T=$(fetch https://cairographics.org/releases/pixman-$V.tar.gz)
S=$PORTS_SRC/pixman-$V; [ -d "$S" ] || tar xzf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/pixman" -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled -Dopenmp=disabled
echo "pixman $V installed"
