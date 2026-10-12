#!/bin/sh
# libpng (cairo PNG functions: swaybar icons, swaybg)
. "$(dirname "$0")/common.sh"
V=1.6.44
T=$(fetch https://download.sourceforge.net/libpng/libpng-$V.tar.xz)
rm -rf "$PORTS_BUILD/libpng-$V"; tar xJf "$T" -C "$PORTS_BUILD"
autotools_port "$PORTS_BUILD/libpng-$V" --disable-static --disable-tools --enable-hardware-optimizations=no \
    CPPFLAGS="-I$PREFIX_ROOT/usr/include" LDFLAGS="-L$PREFIX_ROOT/usr/lib"
cleanup_la
rm -f "$PREFIX_ROOT"/usr/bin/png-fix-itxt "$PREFIX_ROOT"/usr/bin/pngfix
echo "libpng $V installed"
