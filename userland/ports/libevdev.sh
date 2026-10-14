#!/bin/sh
. "$(dirname "$0")/common.sh"
V=1.13.3
# freedesktop.org repeatedly times out in CI. Alpine mirrors the identical release
# archive (verified byte-for-byte); pin its SHA256 and retain upstream as fallback.
T=$DL/libevdev-$V.tar.xz
"$UL/fetch.sh" "$T" \
    "https://distfiles.alpinelinux.org/distfiles/v3.21/libevdev-$V.tar.xz" \
    "https://www.freedesktop.org/software/libevdev/libevdev-$V.tar.xz"
printf '%s  %s\n' abf1aace86208eebdd5d3550ffded4c8d73bb405b796d51c389c9d0604cbcfbf "$T" | sha256sum -c -
S=$PORTS_SRC/libevdev-$V; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/libevdev" -Dtests=disabled -Ddocumentation=disabled -Dcoverity=false
echo "libevdev $V installed"
