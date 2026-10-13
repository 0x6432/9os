#!/bin/sh
# foot: Wayland terminal (+ footclient); terminfo from the source tree's foot.info is not compiled (no tic)
. "$(dirname "$0")/common.sh"
V=1.19.0
T=$(fetch https://codeberg.org/dnkl/foot/archive/$V.tar.gz foot-$V.tar.gz)
S=$PORTS_SRC/foot; [ -d "$S" ] || tar xzf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/foot" --native-file "$(wl_native)" -Ddocs=disabled -Dterminfo=disabled \
    -Dgrapheme-clustering=disabled -Dthemes=false -Dtests=false -Dutmp-backend=none -Dime=true
echo "foot $V installed"
