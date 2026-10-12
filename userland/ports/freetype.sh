#!/bin/sh
# FreeType (fontconfig, fcft); zlib built in, no PNG/HarfBuzz/brotli
. "$(dirname "$0")/common.sh"
V=2.13.3
T=$(fetch https://download.savannah.gnu.org/releases/freetype/freetype-$V.tar.xz)
S=$PORTS_SRC/freetype-$V; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/freetype" -Dbrotli=disabled -Dbzip2=disabled -Dharfbuzz=disabled -Dpng=disabled -Dzlib=internal -Dtests=disabled
echo "freetype $V installed"
