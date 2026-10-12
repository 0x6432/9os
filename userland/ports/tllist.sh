#!/bin/sh
# tllist: typed linked list header (fcft, foot)
. "$(dirname "$0")/common.sh"
V=1.1.0
T=$(fetch https://codeberg.org/dnkl/tllist/archive/$V.tar.gz tllist-$V.tar.gz)
S=$PORTS_SRC/tllist; [ -d "$S" ] || tar xzf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/tllist"
echo "tllist $V installed"
