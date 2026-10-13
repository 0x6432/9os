#!/bin/sh
# M38: software OpenGL/GLES2 + EGL/GBM, Wayland/surfaceless/DRM; no LLVM or hardware DRI.
. "$(dirname "$0")/common.sh"
V=24.3.4
T=$(fetch "https://archive.mesa3d.org/mesa-$V.tar.xz")
S=$PORTS_SRC/mesa-$V; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
N=$(wl_native)
meson_port "$S" "$PORTS_BUILD/mesa" \
    --native-file "$N" \
    -Dplatforms=wayland -Dgallium-drivers=softpipe -Dvulkan-drivers= -Dllvm=disabled \
    -Degl=enabled -Dgbm=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
    -Dglx=disabled -Dglvnd=disabled -Dshared-glapi=enabled \
    -Dgallium-vdpau=disabled -Dgallium-va=disabled -Dgallium-xa=disabled \
    -Dgallium-nine=false -Dgallium-rusticl=false -Dgallium-opencl=disabled \
    -Dshader-cache=disabled -Dzlib=disabled -Dzstd=disabled -Dlibunwind=disabled \
    -Dbuild-tests=false -Dcpp_args="['-I$PREFIX_ROOT/usr/include', '-fno-exceptions', '-fno-rtti']"
echo "mesa $V softpipe installed"
