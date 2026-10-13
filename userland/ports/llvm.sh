#!/bin/sh
# M38: musl LLVM shared JIT library, only the guest architecture backend.
. "$(dirname "$0")/common.sh"
V=18.1.8
S=$PORTS_SRC/llvm-jit-$V/llvm
if [ ! -f "$S/.unpacked" ]; then
    T=$(fetch "https://github.com/llvm/llvm-project/releases/download/llvmorg-$V/llvm-$V.src.tar.xz")
    mkdir -p "$S"; tar xJf "$T" -C "$S" --strip-components=1
    touch "$S/.unpacked"
fi
C=$PORTS_SRC/llvm-jit-$V/cmake
if [ ! -f "$C/.unpacked" ]; then
    T=$(fetch "https://github.com/llvm/llvm-project/releases/download/llvmorg-$V/cmake-$V.src.tar.xz")
    mkdir -p "$C"; tar xJf "$T" -C "$C" --strip-components=1; touch "$C/.unpacked"
fi
case $ARCH in x86_64) TARGET=X86;; aarch64) TARGET=AArch64;; riscv64) TARGET=RISCV;; esac
B=$PORTS_BUILD/llvm
# TableGen is executed on the build machine, never in the guest.
TBLGEN=${LLVM_TABLEGEN:-$(command -v llvm-tblgen)}
cmake -S "$S" -B "$B" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR="$ARCH" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_C_COMPILER="$DCCBIN" -DCMAKE_CXX_COMPILER="$UL/musl-dcxx" \
    -DCMAKE_AR="$(command -v llvm-ar)" -DCMAKE_RANLIB="$(command -v llvm-ranlib)" \
    -DCMAKE_STRIP="$(command -v llvm-strip)" -DLLVM_PARALLEL_LINK_JOBS=1 \
    -DLLVM_HOST_TRIPLE="$ARCH-unknown-linux-musl" -DLLVM_DEFAULT_TARGET_TRIPLE="$ARCH-unknown-linux-musl" \
    -DLLVM_TARGETS_TO_BUILD="$TARGET" -DLLVM_TABLEGEN="$TBLGEN" \
    -DLLVM_NATIVE_TOOL_DIR="$(dirname "$TBLGEN")" \
    -DLLVM_BUILD_LLVM_DYLIB=ON -DLLVM_LINK_LLVM_DYLIB=ON \
    '-DLLVM_DYLIB_COMPONENTS=bitwriter;interpreter;mcdisassembler;mcjit;core;executionengine;scalaropts;transformutils;instcombine;coroutines;native;orcjit;lto;passes;target;analysis' \
    -DLLVM_ENABLE_EH=OFF -DLLVM_ENABLE_RTTI=OFF -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF \
    -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
    -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_BACKTRACES=OFF \
    -DCMAKE_C_FLAGS="-I$PREFIX_ROOT/usr/include" \
    -DCMAKE_CXX_FLAGS="-I$PREFIX_ROOT/usr/include" \
    -DCMAKE_EXE_LINKER_FLAGS="-L$PREFIX_ROOT/usr/lib -Wl,-rpath-link,$PREFIX_ROOT/usr/lib" \
    -DCMAKE_SHARED_LINKER_FLAGS="-L$PREFIX_ROOT/usr/lib -Wl,-rpath-link,$PREFIX_ROOT/usr/lib" \
    > "$B.log" 2>&1 || { tail -40 "$B.log"; exit 1; }
ninja -C "$B" -j"${JOBS:-2}" LLVM >> "$B.log" 2>&1 || { tail -40 "$B.log"; exit 1; }
DESTDIR="$PREFIX_ROOT" cmake --install "$B" --component LLVM --strip >> "$B.log" 2>&1
DESTDIR="$PREFIX_ROOT" cmake --install "$B" --component llvm-headers >> "$B.log" 2>&1
# LLVM 18's config tool checks the compatibility SONAME in the development tree;
# CMake creates that alias only during installation. Point it at the target DSO.
ln -sf "$PREFIX_ROOT/usr/lib/libLLVM-18.so" "$B/lib/libLLVM-18.so"
# A host-executable llvm-config carrying the TARGET's generated metadata.
# It only queries configuration; the target library is never loaded on the host.
mkdir -p "$B/host-bin"
clang++ -std=c++17 -fno-rtti -I"$B/include" -I"$S/include" -I"$B/tools/llvm-config" \
    "$S/tools/llvm-config/llvm-config.cpp" \
    -L"$(llvm-config --libdir)" -Wl,-rpath,"$(llvm-config --libdir)" \
    $(llvm-config --libs support targetparser --system-libs) -o "$B/host-bin/llvm-config" \
    >> "$B.log" 2>&1 || { tail -40 "$B.log"; exit 1; }
# This location is recognized as the target development tree: queries return
# its source/generated includes and built musl library, not host LLVM paths.
echo "LLVM $V $TARGET shared JIT installed"
