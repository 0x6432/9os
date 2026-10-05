#!/bin/sh
# Build compiler-rt builtins (libclang_rt.builtins.a) for a non-x86 ARCH into the musl sysroot.
# Needed for 128-bit long double soft-float (__addtf3 & co.) used by musl on riscv64/aarch64.
set -e
ARCH=${ARCH:-riscv64}
VER=15.0.7
TOP=$(cd "$(dirname "$0")" && pwd)
SRC=$TOP/build/compiler-rt-$VER.src
mkdir -p "$TOP/build"
if [ ! -d "$SRC" ]; then
    T=$TOP/build/compiler-rt-$VER.src.tar.xz
    [ -f ${TOOLS_DIR:-/data/tools}/compiler-rt-$VER.src.tar.xz ] && T=${TOOLS_DIR:-/data/tools}/compiler-rt-$VER.src.tar.xz
    [ -f "$T" ] || curl -sL -o "$T" https://github.com/llvm/llvm-project/releases/download/llvmorg-$VER/compiler-rt-$VER.src.tar.xz
    (cd "$TOP/build" && tar xf "$T")
fi
SYSROOT=$TOP/sysroot/$ARCH
B=$TOP/build/crt-$ARCH
rm -rf "$B" && mkdir -p "$B"
EXTRA=
[ "$ARCH" = aarch64 ] && EXTRA="$SRC/lib/builtins/aarch64/fp_mode.c"
DEFS=
[ "$ARCH" = riscv64 ] && DEFS="-D__NR_riscv_flush_icache=259"
for f in "$SRC"/lib/builtins/*.c $EXTRA; do
    n=$(basename "$f" .c)
    case $n in
        apple_versioning|atomic|atomic_*|emutls|enable_execute_stack|eprintf|gcc_personality_v0|os_version_check|trampoline_setup|cpu_model) continue;;
    esac
    clang --target=$ARCH-linux-musl -O2 -fno-stack-protector -ffreestanding -fPIC -nostdinc $DEFS \
        -isystem "$SYSROOT/include" -isystem "$(clang -print-resource-dir)/include" \
        -c "$f" -o "$B/$n.o" 2>/dev/null || echo "skip $n"
    [ "$n" = clear_cache ] && [ ! -f "$B/$n.o" ] && { echo "compiler-rt: clear_cache.c failed (missing UAPI headers?)" >&2; exit 1; }
done
llvm-ar rcs "$SYSROOT/lib/libclang_rt.builtins.a" "$B"/*.o
echo "compiler-rt builtins: $(ls "$B" | wc -l) objects -> $SYSROOT/lib/libclang_rt.builtins.a"
