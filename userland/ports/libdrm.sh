#!/bin/sh
. "$(dirname "$0")/common.sh"
V=2.4.123
T=$(fetch https://dri.freedesktop.org/libdrm/libdrm-$V.tar.xz)
S=$PORTS_SRC/libdrm-$V; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/libdrm" -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled \
    -Dvmwgfx=disabled -Domap=disabled -Dexynos=disabled -Dfreedreno=disabled -Dtegra=disabled -Dvc4=disabled \
    -Detnaviv=disabled -Dcairo-tests=disabled -Dman-pages=disabled -Dvalgrind=disabled -Dtests=true -Dinstall-test-programs=true
echo "libdrm $V installed"
