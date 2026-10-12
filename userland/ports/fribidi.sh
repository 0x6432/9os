#!/bin/sh
# GNU FriBidi: Unicode bidirectional algorithm (Pango)
. "$(dirname "$0")/common.sh"
V=1.0.16
T=$(fetch https://github.com/fribidi/fribidi/releases/download/v$V/fribidi-$V.tar.xz)
S=$PORTS_SRC/fribidi-$V; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/fribidi" -Ddocs=false -Dbin=false -Dtests=false
echo "fribidi $V installed"
