#!/bin/sh
# hwdata: PNP/PCI/USB id databases (pnp.ids for libdisplay-info and wlroots), data only
. "$(dirname "$0")/common.sh"
V=0.390
T=$(fetch https://github.com/vcrhonek/hwdata/archive/refs/tags/v$V.tar.gz hwdata-$V.tar.gz)
S=$PORTS_SRC/hwdata-$V; [ -d "$S" ] || tar xzf "$T" -C "$PORTS_SRC"
mkdir -p "$PREFIX_ROOT/usr/share/hwdata" "$PREFIX_ROOT/usr/share/pkgconfig"
cp "$S/pnp.ids" "$S/pci.ids" "$S/usb.ids" "$PREFIX_ROOT/usr/share/hwdata/"
cat > "$PREFIX_ROOT/usr/share/pkgconfig/hwdata.pc" <<EOP
prefix=/usr
datadir=\${prefix}/share
pkgdatadir=\${datadir}/hwdata
Name: hwdata
Description: Hardware identification and configuration data
Version: $V
EOP
# build-machine copy for meson's native dependency('hwdata') (-Dbuild.pkg_config_path=$NATIVE_PC)
mkdir -p "$NATIVE_PC"
sed "s|^prefix=.*|prefix=$PREFIX_ROOT/usr|" "$PREFIX_ROOT/usr/share/pkgconfig/hwdata.pc" > "$NATIVE_PC/hwdata.pc"
echo "hwdata $V installed"
