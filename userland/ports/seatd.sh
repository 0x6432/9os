#!/bin/sh
. "$(dirname "$0")/common.sh"
V=0.9.1
# Alpine's cached archive is byte-identical to the upstream 0.9.1 release.
# Keep the upstream URL first, with a mirror for intermittent sr.ht outages.
T=$DL/seatd-$V.tar.gz
"$UL/fetch.sh" "$T" "https://git.sr.ht/~kennylevinsen/seatd/archive/$V.tar.gz" \
    "https://distfiles.alpinelinux.org/distfiles/v3.22/seatd-$V.tar.gz"
S=$PORTS_SRC/seatd-$V; [ -d "$S" ] || tar xzf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/seatd" -Dlibseat-logind=disabled -Dlibseat-seatd=enabled -Dlibseat-builtin=enabled \
    -Dserver=enabled -Dexamples=disabled -Dman-pages=disabled -Dwerror=false -Ddefaultpath=/run/seatd.sock
echo "seatd $V installed"
