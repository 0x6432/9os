#!/bin/sh
# libwayland-client/server/cursor/egl (cross) + a host wayland-scanner in ${TOOLS_DIR:-/data/tools}/host.
. "$(dirname "$0")/common.sh"
V=1.23.1
T=$(fetch https://gitlab.freedesktop.org/wayland/wayland/-/releases/$V/downloads/wayland-$V.tar.xz)
S=$PORTS_SRC/wayland-$V
[ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
HOST=${HOST_TOOLS:-${TOOLS_DIR:-/data/tools}/host}
if [ ! -x "$HOST/bin/wayland-scanner" ]; then
    (unset PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR
     rm -rf "$PORTS_SRC/wayland-host"
     meson setup "$PORTS_SRC/wayland-host" "$S" --prefix="$HOST" -Dlibraries=false -Ddocumentation=false -Dtests=false -Ddtd_validation=false >/dev/null
     ninja -C "$PORTS_SRC/wayland-host" install >/dev/null)
fi
export PATH="$HOST/bin:$PATH"
NATIVE=$PORTS_BUILD/meson-native.txt
PCD=$(dirname "$(find "$HOST" -name wayland-scanner.pc | head -1)")
printf "[binaries]\nwayland-scanner = '%s'\n[built-in options]\npkg_config_path = '%s'\n" "$HOST/bin/wayland-scanner" "$PCD" > "$NATIVE"
meson_port "$S" "$PORTS_BUILD/wayland" --native-file "$NATIVE" -Dscanner=false -Ddocumentation=false -Dtests=false -Ddtd_validation=false
echo "wayland $V installed"
