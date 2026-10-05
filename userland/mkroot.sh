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
# test programs and demos: every userland/{tests,demos}/*.c becomes /bin/<name>
for src in "$TOP"/tests/*.c "$TOP"/demos/*.c; do
    [ -f "$src" ] || continue
    n=$(basename "$src" .c)
    [ "$n" = hello ] && continue
    "$TOP/musl-cc" -O2 -o "bin/$n" "$src" -lm
done
# dynamic linking: musl's libc.so is also its dynamic linker
if [ -f "$TOP/sysroot/$ARCH/lib/libc.so" ]; then
    mkdir -p lib
    cp "$TOP/sysroot/$ARCH/lib/libc.so" lib/libc.so && llvm-strip lib/libc.so
    ln -sf libc.so "lib/ld-musl-$ARCH.so.1"
    for src in "$TOP"/dynlib/*.c; do
        [ -f "$src" ] || continue
        o=$(mktemp).o
        "$TOP/musl-cc" -fPIC -O2 -c -o "$o" "$src"
        "$TOP/musl-cc" -shared -nostdlib -o "lib/$(basename "$src" .c).so" "$o" -L"$TOP/sysroot/$ARCH/lib" -lc
        rm -f "$o"
    done
    # dynamic test programs: userland/dyntests/*.c → /bin/<name> (linked against libc.so)
    for src in "$TOP"/dyntests/*.c; do
        [ -f "$src" ] || continue
        DYNAMIC=1 "$TOP/musl-cc" -O2 -o "bin/$(basename "$src" .c)" "$src" -L"$R/lib" -ldemo -lm
    done
fi
cp -r "$TOP/skel/." "$R/"
echo "root populated: $(find . -type f | wc -l) files, $(du -sk . | cut -f1) KiB"
