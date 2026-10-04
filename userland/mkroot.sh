#!/bin/sh
# Populate userland/root (becomes the initramfs) from built binaries.
set -e
TOP=$(cd "$(dirname "$0")" && pwd)
R=$TOP/root
ARCH=${ARCH:-x86_64}
rm -rf "$R" && mkdir -p "$R"
cd "$R"
mkdir -p bin sbin usr/bin usr/sbin etc root tmp proc sys dev home var/log
cp "$TOP/build/busybox-1.36.1/busybox" bin/busybox
llvm-strip bin/busybox
# applet links (list obtained from the binary itself when it can run on the host)
if [ "$ARCH" = "$(uname -m)" ]; then LIST=$(./bin/busybox --list-full); else LIST=$(cat "$TOP/busybox.links"); fi
for a in $LIST; do
    [ -e "$a" ] || ln -s /bin/busybox "$a"
done
[ -x "$TOP/build/bash/bash" ] && cp "$TOP/build/bash/bash" bin/bash && llvm-strip bin/bash
"$TOP/musl-cc" -O2 -o bin/libctest "$TOP/tests/libctest.c"
cp -r "$TOP/skel/." "$R/"
echo "root populated: $(find . -type f | wc -l) files, $(du -sk . | cut -f1) KiB"
