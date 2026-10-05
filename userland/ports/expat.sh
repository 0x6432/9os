#!/bin/sh
. "$(dirname "$0")/common.sh"
V=2.6.4
T=$(fetch https://github.com/libexpat/libexpat/releases/download/R_$(echo $V | tr . _)/expat-$V.tar.gz)
rm -rf "$PORTS_BUILD/expat-$V"; tar xzf "$T" -C "$PORTS_BUILD"
autotools_port "$PORTS_BUILD/expat-$V" --disable-static --without-docbook --without-examples --without-tests --without-xmlwf
cleanup_la
echo "expat $V installed"
