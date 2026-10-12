#!/bin/sh
# HarfBuzz text shaping (Pango). C++, but built without exceptions/RTTI and linked as C,
# so only the libc++ headers are needed (libcxx-headers.sh)
. "$(dirname "$0")/common.sh"
V=10.1.0
T=$(fetch https://github.com/harfbuzz/harfbuzz/releases/download/$V/harfbuzz-$V.tar.xz)
S=$PORTS_SRC/harfbuzz-$V; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/harfbuzz" -Dglib=enabled -Dgobject=disabled -Dfreetype=enabled -Dcairo=disabled \
    -Dicu=disabled -Dgraphite2=disabled -Dchafa=disabled -Dintrospection=disabled -Dtests=disabled -Ddocs=disabled \
    -Dutilities=disabled -Dbenchmark=disabled -Dwith_libstdcxx=false
echo "harfbuzz $V installed"
