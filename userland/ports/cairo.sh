#!/bin/sh
# cairo 2D graphics (image surfaces + FreeType/fontconfig fonts; no X11, no PNG)
. "$(dirname "$0")/common.sh"
V=1.18.4
T=$(fetch https://cairographics.org/releases/cairo-$V.tar.xz)
S=$PORTS_SRC/cairo-$V; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/cairo" -Dxlib=disabled -Dxcb=disabled -Dxlib-xcb=disabled -Dquartz=disabled -Ddwrite=disabled \
    -Dpng=disabled -Dfreetype=enabled -Dfontconfig=enabled -Dglib=enabled -Dzlib=disabled \
    -Dspectre=disabled -Dsymbol-lookup=disabled -Dtests=disabled -Dgtk_doc=false
echo "cairo $V installed"
