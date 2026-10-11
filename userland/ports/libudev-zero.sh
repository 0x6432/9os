#!/bin/sh
# libudev-zero: libudev on top of /sys (M34) instead of a udev daemon. Its "udev" monitor
# listens on NETLINK_KOBJECT_UEVENT group 4, where udevshim re-sends events (kernel format,
# plus udev properties) after it has created nodes and links. Patched to read udevshim's
# database (/run/udev/data: properties, devlinks, tags, "initialized").
. "$(dirname "$0")/common.sh"
V=1.0.3
T=$(fetch https://github.com/illiliti/libudev-zero/archive/refs/tags/$V.tar.gz libudev-zero-$V.tar.gz)
S=$PORTS_SRC/libudev-zero-$V
[ -d "$S" ] || { tar xzf "$T" -C "$PORTS_SRC" && (cd "$S" && GIT_DIR=/nonexistent git apply -p1 "$UL/ports/patches/libudev-zero-udevdb.patch"); }
(cd "$S" && make clean >/dev/null && make -j"$(nproc)" CC="$DCCBIN" AR=llvm-ar CFLAGS=-O2 PREFIX=/usr libudev.so.1 libudev.pc >make.out 2>&1 \
    && make PREFIX=/usr DESTDIR="$PREFIX_ROOT" install-shared >>make.out 2>&1) || { tail -30 "$S/make.out"; exit 1; }
echo "libudev-zero $V installed"
