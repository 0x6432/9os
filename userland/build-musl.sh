#!/bin/sh
# Build a static musl sysroot for ARCH (default x86_64) with clang.
set -e
ARCH=${ARCH:-x86_64}
MUSL_VER=1.2.5
TOP=$(cd "$(dirname "$0")" && pwd)
SRC=$TOP/build/musl-$MUSL_VER
SYSROOT=$TOP/sysroot/$ARCH
mkdir -p "$TOP/build"
if [ ! -d "$SRC" ]; then
    T=${TOOLS_DIR:-/data/tools}/musl-$MUSL_VER.tar.gz
    [ -f "$T" ] || { T=${TOOLS_DIR:-/data/tools}/dl/musl-$MUSL_VER.tar.gz
        "$TOP/fetch.sh" "$T" https://musl.libc.org/releases/musl-$MUSL_VER.tar.gz \
            https://git.musl-libc.org/cgit/musl/snapshot/musl-$MUSL_VER.tar.gz; }
    (cd "$TOP/build" && tar xzf "$T")
fi
B=$TOP/build/musl-$ARCH
rm -rf "$B" && mkdir -p "$B" && cd "$B"
CC="clang --target=$ARCH-linux-musl" AR=llvm-ar RANLIB=llvm-ranlib \
    CFLAGS="-O2 -fno-stack-protector" \
    "$SRC/configure" --target=$ARCH-linux-musl --prefix="$SYSROOT" --disable-shared >/dev/null
make -j"$(nproc)" >/dev/null
make install >/dev/null
echo "musl installed to $SYSROOT"
