#!/bin/sh
# Pango text layout (+pangocairo, pangoft2) for sway/swaybar
. "$(dirname "$0")/common.sh"
V=1.54.0
T=$(fetch https://download.gnome.org/sources/pango/${V%.*}/pango-$V.tar.xz)
S=$PORTS_SRC/pango-$V; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/pango" -Dintrospection=disabled -Dbuild-testsuite=false -Dbuild-examples=false \
    -Dfontconfig=enabled -Dfreetype=enabled -Dcairo=enabled -Dxft=disabled -Dlibthai=disabled -Dsysprof=disabled \
    -Ddocumentation=false -Dgtk_doc=false
echo "pango $V installed"
