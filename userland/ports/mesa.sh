#!/bin/sh
# M38: software OpenGL/GLES2 + EGL/GBM. Optional musl LLVM/llvmpipe JIT.
. "$(dirname "$0")/common.sh"
V=24.3.4
T=$(fetch "https://archive.mesa3d.org/mesa-$V.tar.xz")
S=$PORTS_SRC/mesa-$V; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
N=$(wl_native)
DRIVERS=softpipe; LLVMOPT=disabled
if [ "${MESA_LLVM:-0}" = 1 ]; then
    DRIVERS=softpipe,llvmpipe; LLVMOPT=enabled
    CONFIG=$PORTS_BUILD/llvm/host-bin/llvm-config
    [ -x "$CONFIG" ] || { echo "mesa: build the llvm port first" >&2; exit 1; }
    sed -i "/^\[binaries\]$/a llvm-config = '$CONFIG'" "$CROSS"
    # Mesa's ORC cache uses dynamic_cast, but LLVM/libc++ are no-RTTI builds.
    # Pin the zero-thread compiler: LLVM 18 constructs TMOwningSimpleCompiler,
    # a SimpleCompiler subclass. The checked source replacement is then safe.
    python3 "$UL/ports/mesa-orc-no-rtti.py" "$S"
fi
meson_port "$S" "$PORTS_BUILD/mesa" \
    --native-file "$N" \
    -Dplatforms=wayland -Dgallium-drivers="$DRIVERS" -Dvulkan-drivers= -Dllvm="$LLVMOPT" \
    -Dshared-llvm=enabled -Dcpp_rtti=false -Dllvm-orcjit=true \
    -Degl=enabled -Dgbm=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
    -Dglx=disabled -Dglvnd=disabled -Dshared-glapi=enabled \
    -Dgallium-vdpau=disabled -Dgallium-va=disabled -Dgallium-xa=disabled \
    -Dgallium-nine=false -Dgallium-rusticl=false -Dgallium-opencl=disabled \
    -Dshader-cache=disabled -Dzlib=disabled -Dzstd=disabled -Dlibunwind=disabled \
    -Dbuild-tests=false -Dcpp_args="['-I$PREFIX_ROOT/usr/include', '-fno-exceptions', '-fno-rtti']"
echo "mesa $V $DRIVERS installed"
