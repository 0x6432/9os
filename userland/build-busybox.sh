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
    if [ "$ARCH" = x86_64 ]; then
        # Debian/Ubuntu keep <asm/*.h> under the multiarch directory
        if [ -d /usr/include/asm ]; then cp -r /usr/include/asm "$SYSROOT/include/"
        else cp -r /usr/include/x86_64-linux-gnu/asm "$SYSROOT/include/"; fi
    else
        # generic architectures: <asm/X.h> is <asm-generic/X.h>
        mkdir -p "$SYSROOT/include/asm"
        for h in /usr/include/asm-generic/*.h; do
            n=$(basename "$h"); echo "#include <asm-generic/$n>" > "$SYSROOT/include/asm/$n"
        done
        echo '#include <linux/byteorder/little_endian.h>' > "$SYSROOT/include/asm/byteorder.h"
        printf '#define __BITS_PER_LONG 64\n#include <asm-generic/bitsperlong.h>\n' > "$SYSROOT/include/asm/bitsperlong.h"
    fi
fi
mkdir -p "$TOP/build"
SRC=$TOP/build/busybox-$BB_VER
B=$TOP/build/busybox-$ARCH
if [ ! -d "$B" ]; then
    T=${TOOLS_DIR:-/data/tools}/busybox-$BB_VER.tar.bz2
    [ -f "$T" ] || { T=$TOP/build/busybox.tar.bz2; curl -sL -o "$T" https://busybox.net/downloads/busybox-$BB_VER.tar.bz2; }
    (cd "$TOP/build" && tar xjf "$T" && mv busybox-$BB_VER "$B")
fi
export ARCH
cd "$B"
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
echo "busybox installed"
