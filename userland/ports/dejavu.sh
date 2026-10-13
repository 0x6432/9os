#!/bin/sh
# DejaVu TTF fonts (/usr/share/fonts/dejavu)
. "$(dirname "$0")/common.sh"
V=2.37
T=$(fetch https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_${V%.*}_${V#*.}/dejavu-fonts-ttf-$V.tar.bz2)
S=$PORTS_SRC/dejavu-fonts-ttf-$V; [ -d "$S" ] || tar xjf "$T" -C "$PORTS_SRC"
mkdir -p "$PREFIX_ROOT/usr/share/fonts/dejavu"
cp "$S"/ttf/DejaVuSans.ttf "$S"/ttf/DejaVuSans-Bold.ttf "$S"/ttf/DejaVuSansMono*.ttf "$S"/ttf/DejaVuSerif.ttf "$PREFIX_ROOT/usr/share/fonts/dejavu/"
echo "dejavu $V installed"
