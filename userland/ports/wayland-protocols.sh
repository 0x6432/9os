#!/bin/sh
. "$(dirname "$0")/common.sh"
V=1.38
T=$(fetch https://gitlab.freedesktop.org/wayland/wayland-protocols/-/releases/$V/downloads/wayland-protocols-$V.tar.xz)
S=$PORTS_SRC/wayland-protocols-$V; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
HOST=${HOST_TOOLS:-${TOOLS_DIR:-/data/tools}/host}; export PATH="$HOST/bin:$PATH"
PCD=$(dirname "$(find "$HOST" -name wayland-scanner.pc | head -1)")
NATIVE=$PORTS_BUILD/meson-native.txt
printf "[binaries]\nwayland-scanner = '%s'\n[built-in options]\npkg_config_path = '%s'\n" "$HOST/bin/wayland-scanner" "$PCD" > "$NATIVE"
meson_port "$S" "$PORTS_BUILD/wayland-protocols" --native-file "$NATIVE" -Dtests=false
echo "wayland-protocols $V installed"
