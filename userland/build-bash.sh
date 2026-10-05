#!/bin/sh
# Build a static Bash against the 9os musl sysroot.
set -e
ARCH=${ARCH:-x86_64}
VER=5.2.37
TOP=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$TOP/build"
SRC=$TOP/build/bash-$VER
[ -d "$SRC" ] || (cd "$TOP/build" && { [ -f ${TOOLS_DIR:-/data/tools}/bash-$VER.tar.gz ] && tar xzf ${TOOLS_DIR:-/data/tools}/bash-$VER.tar.gz || curl -sL https://ftp.gnu.org/gnu/bash/bash-$VER.tar.gz | tar xz; })
B=$TOP/build/bash-$ARCH
rm -rf "$B" && mkdir -p "$B" && cd "$B"
export ARCH
HOST=""
[ "$ARCH" = "$(uname -m)" ] || HOST="--host=$ARCH-linux-musl"
CC="$TOP/musl-cc" CC_FOR_BUILD=gcc AR=llvm-ar RANLIB=llvm-ranlib CFLAGS="-O2 -Wno-implicit-function-declaration -Wno-int-conversion -std=gnu17" \
    "$SRC/configure" $HOST --without-bash-malloc --enable-static-link --disable-nls \
    --without-curses --enable-job-control --enable-readline >/dev/null
make -j"$(nproc)" >/dev/null 2>&1 || make 2>&1 | grep -E "error" | head -20
ls -la bash
