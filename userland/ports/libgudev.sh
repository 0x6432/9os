#!/bin/sh
# libgudev: GObject wrapper of libudev (libudev-zero here), needed by libwacom
. "$(dirname "$0")/common.sh"
V=238
T=$(fetch https://download.gnome.org/sources/libgudev/$V/libgudev-$V.tar.xz)
S=$PORTS_SRC/libgudev-$V; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/libgudev" -Dintrospection=disabled -Dvapi=disabled -Dtests=disabled -Dgtk_doc=false
echo "libgudev $V installed"
