#!/bin/sh
# Populate userland/root-$ARCH (becomes the initramfs) from built binaries.
set -e
TOP=$(cd "$(dirname "$0")" && pwd)
ARCH=${ARCH:-x86_64}
R=$TOP/root-$ARCH
export ARCH
rm -rf "$R" && mkdir -p "$R"
cd "$R"
mkdir -p bin sbin usr/bin usr/sbin etc root tmp proc sys dev home var/log
cp "$TOP/build/busybox-$ARCH/busybox" bin/busybox
llvm-strip bin/busybox
# applet links (list obtained from the binary itself when it can run on the host)
if [ "$ARCH" = "$(uname -m)" ]; then LIST=$(./bin/busybox --list-full); else LIST=$(cat "$TOP/busybox.links"); fi
for a in $LIST; do
    [ -e "$a" ] || ln -s /bin/busybox "$a"
done
if [ -x "$TOP/build/bash-$ARCH/bash" ]; then cp "$TOP/build/bash-$ARCH/bash" bin/bash && llvm-strip bin/bash; fi
"$TOP/musl-cc" -O2 -o bin/libctest "$TOP/tests/libctest.c"
cp -r "$TOP/skel/." "$R/"
echo "root populated: $(find . -type f | wc -l) files, $(du -sk . | cut -f1) KiB"
