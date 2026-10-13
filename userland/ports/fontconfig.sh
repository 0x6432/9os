#!/bin/sh
# fontconfig (needs host gperf); fonts from /usr/share/fonts, cache in /var/cache/fontconfig
. "$(dirname "$0")/common.sh"
V=2.15.0
T=$(fetch https://www.freedesktop.org/software/fontconfig/release/fontconfig-$V.tar.xz)
S=$PORTS_SRC/fontconfig-$V; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/fontconfig" -Ddoc=disabled -Dnls=disabled -Dtests=disabled -Dtools=enabled \
    -Dcache-build=disabled --sysconfdir=/etc --localstatedir=/var
echo "fontconfig $V installed"
