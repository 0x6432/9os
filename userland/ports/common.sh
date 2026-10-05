# Shared helpers for userland/ports/*.sh (sourced). Ports install into $PREFIX_ROOT/usr,
# which mkroot.sh copies into the root filesystem.
set -e
ARCH=${ARCH:-x86_64}; export ARCH
UL=$(cd "$(dirname "$0")/.." && pwd)
SYSROOT=$UL/sysroot/$ARCH
CC="$UL/musl-cc"
DCC="env DYNAMIC=1 $UL/musl-cc"
PORTS_SRC=$UL/build/ports-src
PORTS_BUILD=$UL/build/ports-$ARCH
PREFIX_ROOT=$UL/build/ports-root-$ARCH
DL=${DL:-/data/tools/dl}
mkdir -p "$PORTS_SRC" "$PORTS_BUILD" "$PREFIX_ROOT/usr/bin" "$PREFIX_ROOT/usr/lib" "$PREFIX_ROOT/usr/include" "$DL"
# fetch URL [file]: download once into $DL
fetch() { f=${2:-$(basename "$1")}; [ -s "$DL/$f" ] || curl -sfL -o "$DL/$f" "$1"; echo "$DL/$f"; }
# build a shared library from objects: mkso libname.so obj...
mkso() { so=$1; shift; "$CC" -shared -nostdlib -Wl,-soname,"$so" -o "$so" "$@" -L"$SYSROOT/lib" -lc; }
