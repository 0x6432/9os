#!/bin/sh
# libc++ headers only (no libc++ library): enough for C++ code that avoids the runtime
# (exceptions, RTTI, iostreams), i.e. HarfBuzz, compiled through musl-dcxx
. "$(dirname "$0")/common.sh"
V=18.1.8
T=$(fetch https://github.com/llvm/llvm-project/releases/download/llvmorg-$V/libcxx-$V.src.tar.xz)
S=$PORTS_SRC/libcxx-$V.src; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
D=$PREFIX_ROOT/usr/include/c++/v1
rm -rf "$D"; mkdir -p "$D"
cp -r "$S/include/." "$D/"
rm -f "$D"/CMakeLists.txt "$D"/*.in "$D"/module.modulemap*
cp "$S/vendor/llvm/default_assertion_handler.in" "$D/__assertion_handler"
cat > "$D/__config_site" <<'EOC'
#ifndef _LIBCPP___CONFIG_SITE
#define _LIBCPP___CONFIG_SITE
#define _LIBCPP_ABI_VERSION 1
#define _LIBCPP_ABI_NAMESPACE __1
#define _LIBCPP_HAS_MUSL_LIBC
#define _LIBCPP_HAS_THREAD_API_PTHREAD
#define _LIBCPP_HAS_NO_VENDOR_AVAILABILITY_ANNOTATIONS
#define _LIBCPP_HAS_NO_TIME_ZONE_DATABASE
#define _LIBCPP_HAS_NO_STD_MODULES
#define _LIBCPP_PSTL_CPU_BACKEND_SERIAL
#define _LIBCPP_HARDENING_MODE_DEFAULT _LIBCPP_HARDENING_MODE_NONE
#endif
EOC
echo "libcxx-headers $V installed"
