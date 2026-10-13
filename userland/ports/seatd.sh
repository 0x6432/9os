#!/bin/sh
. "$(dirname "$0")/common.sh"
V=0.9.1
T=$(fetch https://git.sr.ht/~kennylevinsen/seatd/archive/$V.tar.gz seatd-$V.tar.gz)
S=$PORTS_SRC/seatd-$V; [ -d "$S" ] || tar xzf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/seatd" -Dlibseat-logind=disabled -Dlibseat-seatd=enabled -Dlibseat-builtin=enabled \
    -Dserver=enabled -Dexamples=disabled -Dman-pages=disabled -Dwerror=false -Ddefaultpath=/run/seatd.sock
echo "seatd $V installed"
