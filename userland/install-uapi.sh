#!/bin/sh
# Install Linux UAPI headers (<linux/*.h>, <asm/*.h>) into the musl sysroot for ARCH.
# Run before compiler-rt (clear_cache.c needs <asm/unistd.h>) and BusyBox.
set -e
ARCH=${ARCH:-x86_64}
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
