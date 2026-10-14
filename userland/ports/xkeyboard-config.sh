#!/bin/sh
# keyboard layouts (data only) for libxkbcommon: /usr/share/X11/xkb
. "$(dirname "$0")/common.sh"
V=2.43
# Release-tag archive mirrored by Alpine, pinned to Alpine's independent SHA512.
# All 419 files in the former release tarball match this tag source byte-for-byte.
T=$DL/xkeyboard-config-tag-$V.tar.gz
"$UL/fetch.sh" "$T" \
    "https://distfiles.alpinelinux.org/distfiles/v3.21/xkeyboard-config-xkeyboard-config-$V.tar.gz" \
    "https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/archive/xkeyboard-config-$V/xkeyboard-config-xkeyboard-config-$V.tar.gz"
printf '%s  %s\n' 2f46c303d8d4b2848275b670616173cb5fee99c3ea65a031b5a9709f1ea543183a6c022adf8a30c6993ce859307046b2b45f775e4bae6c6967e256c89b45a073 "$T" | sha512sum -c -
S=$PORTS_SRC/xkeyboard-config-$V
if [ ! -d "$S" ]; then mkdir -p "$S"; tar xzf "$T" -C "$S" --strip-components=1; fi
meson_port "$S" "$PORTS_BUILD/xkeyboard-config" -Dxkb-base=/usr/share/X11/xkb -Dcompat-rules=true -Dxorg-rules-symlinks=false
echo "xkeyboard-config $V installed"
