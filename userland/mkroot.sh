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
# test programs, demos and small tools: every userland/{tests,demos,tools}/*.c becomes /bin/<name>
for src in "$TOP"/tests/*.c "$TOP"/demos/*.c "$TOP"/tools/*.c; do
    [ -f "$src" ] || continue
    n=$(basename "$src" .c)
    [ "$n" = hello ] && continue
    "$TOP/musl-cc" -O2 -o "bin/$n" "$src" -lm
done
# M34: udevadm is udevshim's second personality (argv[0])
ln -sf udevshim bin/udevadm
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
# ports (userland/build-ports.sh) install under build/ports-root-$ARCH
[ -d "$TOP/build/ports-root-$ARCH/usr" ] && cp -a "$TOP/build/ports-root-$ARCH/usr/bin" "$TOP/build/ports-root-$ARCH/usr/lib" usr/ 2>/dev/null || true
# M35: helpers and data (libinput tools, xkb layouts, libinput quirks)
P=$TOP/build/ports-root-$ARCH/usr
[ -d "$P/libexec" ] && cp -a "$P/libexec" usr/
mkdir -p usr/share
for d in X11 libinput libwacom drirc.d; do [ -d "$P/share/$d" ] && cp -a "$P/share/$d" usr/share/; done
# M37: fonts and fontconfig, foot's config
for d in fonts fontconfig; do [ -d "$P/share/$d" ] && cp -a "$P/share/$d" usr/share/; done
for d in fonts xdg sway; do [ -d "$P/../etc/$d" ] && mkdir -p etc && cp -a "$P/../etc/$d" etc/; done
[ -d "$P/share/fonts" ] && mkdir -p var/cache/fontconfig
rm -rf usr/lib/pkgconfig usr/lib/cmake
cp -r "$TOP/skel/." "$R/"
# M31: busybox is setuid root (it drops privileges except for su/passwd/login/ping...)
chmod 4755 bin/busybox
chmod 640 etc/shadow
echo "root populated: $(find . -type f | wc -l) files, $(du -sk . | cut -f1) KiB"
