#!/bin/sh
# wlkms (KMS Wayland compositor) + wlclient (xdg-shell demo client)
. "$(dirname "$0")/common.sh"
SCAN=${WAYLAND_SCANNER:-/data/tools/host/bin/wayland-scanner}
XML=$PREFIX_ROOT/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
G=$PORTS_BUILD/wlkms; mkdir -p "$G"
$SCAN server-header "$XML" "$G/xdg-shell-server-protocol.h"
$SCAN client-header "$XML" "$G/xdg-shell-client-protocol.h"
$SCAN private-code "$XML" "$G/xdg-shell-protocol.c"
SRC=$(dirname "$0")/src
INC="-I$G -I$PREFIX_ROOT/usr/include -I$PREFIX_ROOT/usr/include/libdrm -I$PREFIX_ROOT/usr/include/pixman-1"
LNK="-L$PREFIX_ROOT/usr/lib -Wl,-rpath-link,$PREFIX_ROOT/usr/lib"
$DCC -O2 $INC -o "$PREFIX_ROOT/usr/bin/wlkms" "$SRC/wlkms.c" "$G/xdg-shell-protocol.c" $LNK -lwayland-server -ldrm -lpixman-1
$DCC -O2 $INC -o "$PREFIX_ROOT/usr/bin/wlclient" "$SRC/wlclient.c" "$G/xdg-shell-protocol.c" $LNK -lwayland-client
echo "wlkms/wlclient built"
