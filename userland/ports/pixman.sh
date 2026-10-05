#!/bin/sh
. "$(dirname "$0")/common.sh"
V=0.44.2
T=$(fetch https://cairographics.org/releases/pixman-$V.tar.gz)
S=$PORTS_SRC/pixman-$V; [ -d "$S" ] || tar xzf "$T" -C "$PORTS_SRC"
EXTRA=
# RVV kernels need vector state the 9os kernel does not context-switch (and <asm/hwcap.h>)
[ "$ARCH" = riscv64 ] && grep -qs "'rvv'" "$S/meson.options" "$S/meson_options.txt" && EXTRA=-Drvv=disabled
meson_port "$S" "$PORTS_BUILD/pixman" $EXTRA -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled -Dopenmp=disabled
echo "pixman $V installed"
