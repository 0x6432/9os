#!/bin/sh
# libc++ + libc++abi for musl (no exceptions/RTTI/unwinder yet), needed by Mesa.
. "$(dirname "$0")/common.sh"
V=18.1.8
S=$PORTS_SRC/llvm-runtimes-$V
mkdir -p "$S"
for p in llvm cmake runtimes libcxx libcxxabi; do
    [ -f "$S/$p/.unpacked" ] && continue
    T=$(fetch "https://github.com/llvm/llvm-project/releases/download/llvmorg-$V/$p-$V.src.tar.xz")
    mkdir -p "$S/$p"
    if [ "$p" = llvm ]; then
        tar xJf "$T" -C "$S/$p" --strip-components=1 "$p-$V.src/cmake"
    else
        tar xJf "$T" -C "$S/$p" --strip-components=1
    fi
    touch "$S/$p/.unpacked"
done
B=$PORTS_BUILD/libcxx
# The build generates its own libc++ headers. Do not put the installed copy in
# include_next's path, and do not link a previously installed runtime into itself.
export CXXINC= CXX_RUNTIME=0
cmake -S "$S/runtimes" -B "$B" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR="$ARCH" \
    -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_C_COMPILER="$DCCBIN" -DCMAKE_CXX_COMPILER="$UL/musl-dcxx" \
    -DCMAKE_AR="$(command -v llvm-ar)" -DCMAKE_RANLIB="$(command -v llvm-ranlib)" \
    '-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi' -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
    -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBCXX_ENABLE_EXCEPTIONS=OFF -DLIBCXX_ENABLE_RTTI=OFF \
    -DLIBCXXABI_ENABLE_EXCEPTIONS=OFF -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
    -DLIBCXX_HAS_MUSL_LIBC=ON -DLIBCXX_ENABLE_STATIC=OFF -DLIBCXXABI_ENABLE_STATIC=OFF \
    -DLIBCXX_ENABLE_SHARED=ON -DLIBCXXABI_ENABLE_SHARED=ON -DLIBCXX_ENABLE_ABI_LINKER_SCRIPT=OFF \
    -DLIBCXX_ENABLE_TIME_ZONE_DATABASE=OFF -DLIBCXX_INSTALL_LIBRARY_DIR=lib \
    -DLIBCXXABI_INSTALL_LIBRARY_DIR=lib \
    -DLIBCXXABI_HAS_GCC_LIB=OFF -DLIBCXXABI_HAS_GCC_S_LIB=OFF \
    -DLIBCXX_HAS_GCC_S_LIB=OFF -DLIBCXX_HAS_ATOMIC_LIB=OFF > "$B.log" 2>&1 || { tail -30 "$B.log"; exit 1; }
ninja -C "$B" -j"${JOBS:-2}" >> "$B.log" 2>&1 || { tail -30 "$B.log"; exit 1; }
DESTDIR="$PREFIX_ROOT" cmake --install "$B" >> "$B.log" 2>&1 || { tail -30 "$B.log"; exit 1; }
echo "libcxx + libcxxabi $V installed (no exceptions/RTTI)"
