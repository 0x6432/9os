#!/bin/sh
# Populate userland/root-$ARCH (becomes the initramfs) from built binaries.
set -e
TOP=$(cd "$(dirname "$0")" && pwd)
ARCH=${ARCH:-x86_64}
R=$TOP/root-$ARCH
export ARCH
rm -rf "$R" && mkdir -p "$R"
cd "$R"
mkdir -p bin sbin usr/bin usr/sbin usr/lib etc root tmp proc sys dev home var/log
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
# Qt dynamically discovers QPA/image plugins outside usr/lib.
[ -d "$P/plugins" ] && cp -a "$P/plugins" usr/
# QML imports include their qmldir files, scripts and target plugin libraries.
[ -d "$P/qml" ] && cp -a "$P/qml" usr/
mkdir -p usr/share
for d in X11 libinput libwacom drirc.d dbus-1; do [ -d "$P/share/$d" ] && cp -a "$P/share/$d" usr/share/; done
# M37: fonts and fontconfig, foot's config
for d in fonts fontconfig; do [ -d "$P/share/$d" ] && cp -a "$P/share/$d" usr/share/; done
for d in fonts xdg sway dbus-1; do [ -d "$P/../etc/$d" ] && mkdir -p etc && cp -a "$P/../etc/$d" etc/; done
[ -d "$P/share/fonts" ] && mkdir -p var/cache/fontconfig
# Raster QPA without fontconfig searches Qt's libraries-path/fonts by default.
# Share the installed DejaVu data instead of requiring an environment override.
if [ -e usr/plugins/platforms/libqoffscreen.so ] && [ -d usr/share/fonts/dejavu ]; then
    ln -sfn ../share/fonts/dejavu usr/lib/fonts
fi
# Runtime image only: preserve the ports sysroot's development files for linking,
# but do not spend unswappable initramfs pages (or disk-root capacity) on archives,
# qmake metadata, CMake objects or non-runtime ELF symbol tables.
rm -rf usr/lib/pkgconfig usr/lib/cmake
find usr/lib -type f \( -name '*.a' -o -name '*.prl' -o -name '*.la' \) -delete
find usr/lib -type d -name 'objects-*' -prune -exec rm -rf {} +
for d in usr/lib usr/plugins usr/qml; do
    [ -d "$d" ] || continue
    find "$d" -type f -name '*.so*' -exec sh -c '
        for f do
            if [ "$(head -c4 "$f")" = "$(printf "\177ELF")" ]; then
                llvm-strip --strip-unneeded "$f" || exit 1
            fi
        done
    ' sh {} +
done
cp -r "$TOP/skel/." "$R/"
# M31: busybox is setuid root (it drops privileges except for su/passwd/login/ping...)
chmod 4755 bin/busybox
chmod 640 etc/shadow
echo "root populated: $(find . -type f | wc -l) files, $(du -sk . | cut -f1) KiB"
