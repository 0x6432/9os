#!/bin/sh
# Build musl's shared libc.so (= the dynamic linker ld-musl-$ARCH.so.1) into the sysroot.
# Runs after build-musl.sh (and build-compiler-rt.sh on riscv64/aarch64, whose builtins
# libc.so links against).
set -e
ARCH=${ARCH:-x86_64}
MUSL_VER=1.2.5
TOP=$(cd "$(dirname "$0")" && pwd)
SRC=$TOP/build/musl-$MUSL_VER
SYSROOT=$TOP/sysroot/$ARCH
if [ "$ARCH" = x86_64 ]; then RT=$(${LIBGCC_CC:-gcc} -print-libgcc-file-name); else RT=$SYSROOT/lib/libclang_rt.builtins.a; fi
B=$TOP/build/musl-shared-$ARCH
rm -rf "$B" && mkdir -p "$B" && cd "$B"
CC="clang --target=$ARCH-linux-musl" AR=llvm-ar RANLIB=llvm-ranlib LIBCC="$RT" \
    CFLAGS="-O2 -fno-stack-protector" LDFLAGS="-fuse-ld=lld" \
    "$SRC/configure" --target=$ARCH-linux-musl --prefix="$SYSROOT" --disable-static \
    --syslibdir=/lib >/dev/null
make -j"$(nproc)" lib/libc.so >/dev/null
cp lib/libc.so "$SYSROOT/lib/libc.so"
echo "musl libc.so installed to $SYSROOT/lib"
