#!/bin/sh
# Build a static BusyBox against the 9os musl sysroot and install it into userland/root.
set -e
ARCH=${ARCH:-x86_64}
BB_VER=1.36.1
TOP=$(cd "$(dirname "$0")" && pwd)
SYSROOT=$TOP/sysroot/$ARCH
# Linux UAPI headers (BusyBox needs <linux/*.h>); copy from the host for x86_64.
if [ ! -d "$SYSROOT/include/linux" ]; then
    cp -r /usr/include/linux /usr/include/asm-generic /usr/include/mtd "$SYSROOT/include/" 2>/dev/null || true
    [ "$ARCH" = x86_64 ] && cp -r /usr/include/asm "$SYSROOT/include/"
fi
mkdir -p "$TOP/build"
SRC=$TOP/build/busybox-$BB_VER
[ -d "$SRC" ] || (cd "$TOP/build" && curl -sL https://busybox.net/downloads/busybox-$BB_VER.tar.bz2 | tar xj)
cd "$SRC"
make distclean >/dev/null 2>&1 || true
make defconfig >/dev/null
# static, no features that need missing kernel pieces
sed -i -e 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' \
       -e 's/^CONFIG_TC=y/# CONFIG_TC is not set/' \
       -e 's/^CONFIG_FEATURE_TC_INGRESS=y/# CONFIG_FEATURE_TC_INGRESS is not set/' \
       -e 's/^CONFIG_SEEDRNG=y/# CONFIG_SEEDRNG is not set/' .config
yes "" | make oldconfig >/dev/null 2>&1
make -j"$(nproc)" CC="$TOP/musl-cc" HOSTCC=gcc AR=llvm-ar STRIP=llvm-strip \
     CFLAGS="-Wno-error -Wno-ignored-optimization-argument -Wno-unused-command-line-argument" busybox 2>&1 | grep -E "error|Error" | head -20 || true
ls -la busybox
mkdir -p "$TOP/root/bin"
cp busybox "$TOP/root/bin/busybox"
llvm-strip "$TOP/root/bin/busybox"
echo "busybox installed"
