#!/bin/sh
# wlroots: DRM/libinput/session backends, pixman renderer, shm + dumb-buffer allocators (no EGL/GBM yet)
. "$(dirname "$0")/common.sh"
V=0.18.2
T=$(fetch https://gitlab.freedesktop.org/wlroots/wlroots/-/releases/$V/downloads/wlroots-$V.tar.gz)
S=$PORTS_SRC/wlroots-$V; [ -d "$S" ] || tar xzf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/wlroots" --native-file "$(wl_native)" -Dxwayland=disabled -Drenderers=[] \
    -Dallocators=[] -Dbackends=drm,libinput -Dsession=enabled -Dcolor-management=disabled -Dlibliftoff=disabled \
    -Dxcb-errors=disabled -Dexamples=true
cp "$PORTS_BUILD/wlroots/tinywl/tinywl" "$PREFIX_ROOT/usr/bin/" 2>/dev/null || true
echo "wlroots $V installed"
