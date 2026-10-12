#!/bin/sh
# Build the ports listed in $PORTS (default: all userland/ports/*.sh) for ARCH.
set -e
D=$(cd "$(dirname "$0")" && pwd)
# dependency order
ALL="lua sqlite libffi expat wayland wltest wayland-protocols pixman libxkbcommon libdrm wlkms kmstest xkeyboard-config libudev-zero libevdev mtdev check glib libgudev libwacom libinput seatd inputtest hwdata libdisplay-info wlroots freetype fontconfig dejavu tllist fcft foot libcxx-headers fribidi harfbuzz cairo pango json-c sway"
for p in ${PORTS:-$ALL}; do
    echo "== port $p ($ARCH)"; sh "$D/ports/$p.sh"
done
