#!/bin/sh
# Build the ports listed in $PORTS (default: all userland/ports/*.sh) for ARCH.
set -e
D=$(cd "$(dirname "$0")" && pwd)
# dependency order
ALL="lua sqlite libffi expat wayland wltest"
for p in ${PORTS:-$ALL}; do
    echo "== port $p ($ARCH)"; sh "$D/ports/$p.sh"
done
