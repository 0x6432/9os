#!/bin/sh
# fcft: font loading/glyph rasterization for foot (no HarfBuzz shaping yet)
. "$(dirname "$0")/common.sh"
V=3.1.10
T=$(fetch https://codeberg.org/dnkl/fcft/archive/$V.tar.gz fcft-$V.tar.gz)
S=$PORTS_SRC/fcft; [ -d "$S" ] || tar xzf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/fcft" -Dgrapheme-shaping=disabled -Drun-shaping=disabled -Dtest-text-shaping=false -Ddocs=disabled
echo "fcft $V installed"
