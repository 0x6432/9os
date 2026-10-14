#!/bin/sh
# fontconfig (needs host gperf); fonts from /usr/share/fonts, cache in /var/cache/fontconfig
. "$(dirname "$0")/common.sh"
V=2.15.0
# Same upstream release, gzip archive mirrored/pinned by Alpine v3.20. The
# freedesktop endpoint intermittently times out or returns 418 on CI runners.
T=$DL/fontconfig-$V.tar.gz
"$UL/fetch.sh" "$T" \
    "https://distfiles.alpinelinux.org/distfiles/v3.20/fontconfig-$V.tar.gz" \
    "https://www.freedesktop.org/software/fontconfig/release/fontconfig-$V.tar.gz"
printf '%s  %s\n' 4feb02935d94002ed0aba5b9e44bab47c14bbc0ecfcdcdb14c9004681109cef51415cd2368e959f8c4bd2e67ff9e8fcf45102a4bd8728b2f50b3c56723fcb81c "$T" | sha512sum -c -
S=$PORTS_SRC/fontconfig-$V; [ -d "$S" ] || tar xzf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/fontconfig" -Ddoc=disabled -Dnls=disabled -Dtests=disabled -Dtools=enabled \
    -Dcache-build=disabled --sysconfdir=/etc --localstatedir=/var
echo "fontconfig $V installed"
