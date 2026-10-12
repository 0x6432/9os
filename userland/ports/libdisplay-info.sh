#!/bin/sh
# libdisplay-info: EDID/DisplayID parser (wlroots DRM backend)
. "$(dirname "$0")/common.sh"
V=0.1.1
T=$(fetch https://gitlab.freedesktop.org/emersion/libdisplay-info/-/releases/$V/downloads/libdisplay-info-$V.tar.xz)
S=$PORTS_SRC/libdisplay-info-$V; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/libdisplay-info" -Dbuild.pkg_config_path="$NATIVE_PC"
echo "libdisplay-info $V installed"
