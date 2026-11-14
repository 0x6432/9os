#!/bin/sh
# GLib (for libgudev/libwacom). pcre2, zlib and proxy-libintl come from GLib's meson wraps.
. "$(dirname "$0")/common.sh"
V=2.82.4
T=$(fetch https://download.gnome.org/sources/glib/${V%.*}/glib-$V.tar.xz)
S=$PORTS_SRC/glib-$V; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
# Meson verifies the same wrap hash again; never accept the failing mirror bytes.
python3 "$UL/ports/glib-zlib-cache.py" "$S" "$DL"
meson_port "$S" "$PORTS_BUILD/glib" --wrap-mode=default --force-fallback-for=libpcre2-8,zlib -Dtests=false -Dintrospection=disabled -Dlibmount=disabled \
    -Dselinux=disabled -Dxattr=false -Dman-pages=disabled -Ddocumentation=false -Dnls=disabled -Dsysprof=disabled \
    -Dlibelf=disabled -Dglib_debug=disabled -Dpcre2:test=false -Dpcre2:grep=false
rm -rf "$PREFIX_ROOT/usr/share/gdb" "$PREFIX_ROOT/usr/share/glib-2.0/gdb" "$PREFIX_ROOT/usr/share/bash-completion"
echo "glib $V installed"
