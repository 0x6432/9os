#!/bin/sh
# sway 1.10 tiling Wayland compositor (wlroots 0.18): no Xwayland, no tray, no gdk-pixbuf
. "$(dirname "$0")/common.sh"
V=1.10.1
T=$(fetch https://github.com/swaywm/sway/releases/download/$V/sway-$V.tar.gz)
S=$PORTS_SRC/sway-$V; [ -d "$S" ] || tar xzf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/sway" --native-file "$(wl_native)" -Dman-pages=disabled -Dtray=disabled \
    -Dgdk-pixbuf=disabled -Ddefault-wallpaper=false -Dfish-completions=false -Dzsh-completions=false \
    -Dbash-completions=false -Dswaybar=true -Dswaynag=true -Dsd-bus-provider=auto -Dwerror=false
# no wallpaper/swaybg: drop the default background line
sed -i 's|^output \* bg .*|# output * bg <image> fill   (no swaybg on 9os yet)|' "$PREFIX_ROOT/etc/sway/config"
echo "sway $V installed"
