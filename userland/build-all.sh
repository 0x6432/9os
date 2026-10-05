#!/bin/sh
# Build the whole userland (musl, [compiler-rt], BusyBox, Bash) and the root fs for ARCH.
set -e
ARCH=${ARCH:-x86_64}; export ARCH
D=$(cd "$(dirname "$0")" && pwd)
"$D/build-musl.sh"
"$D/install-uapi.sh"
[ "$ARCH" = x86_64 ] || "$D/build-compiler-rt.sh"
"$D/build-musl-shared.sh"
"$D/build-busybox.sh"
"$D/build-bash.sh"
[ -n "$NO_PORTS" ] || "$D/build-ports.sh"
"$D/mkroot.sh"
