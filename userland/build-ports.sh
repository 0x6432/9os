#!/bin/sh
# Build the ports listed in $PORTS (default: all userland/ports/*.sh) for ARCH.
set -e
D=$(cd "$(dirname "$0")" && pwd)
for p in ${PORTS:-$(cd "$D/ports" && ls *.sh | grep -v common.sh | sed 's/\.sh$//')}; do
    echo "== port $p ($ARCH)"; sh "$D/ports/$p.sh"
done
