#!/bin/sh
. "$(dirname "$0")/common.sh"
V=3.4.6
T=$(fetch https://github.com/libffi/libffi/releases/download/v$V/libffi-$V.tar.gz)
rm -rf "$PORTS_BUILD/libffi-$V"; tar xzf "$T" -C "$PORTS_BUILD"
autotools_port "$PORTS_BUILD/libffi-$V" --disable-static --disable-docs --disable-multi-os-directory
cleanup_la
echo "libffi $V installed"
