#!/bin/sh
. "$(dirname "$0")/common.sh"
V=1.1.7
T=$(fetch https://bitmath.org/code/mtdev/mtdev-$V.tar.bz2)
S=$PORTS_SRC/mtdev-$V; [ -d "$S" ] || tar xjf "$T" -C "$PORTS_SRC"
autotools_port "$S" --disable-static
cleanup_la
echo "mtdev $V installed"
