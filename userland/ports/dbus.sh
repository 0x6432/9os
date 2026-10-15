#!/bin/sh
# M40 first slice: shared libdbus and explicit session daemon/tools, no systemd/X11.
. "$(dirname "$0")/common.sh"
V=1.16.2
T=$DL/dbus-$V.tar.xz
"$UL/fetch.sh" "$T" \
    "https://distfiles.alpinelinux.org/distfiles/v3.22/dbus-$V.tar.xz" \
    "https://dbus.freedesktop.org/releases/dbus/dbus-$V.tar.xz"
printf '%s  %s\n' 5c26f52d85984bb9ae1dde8d7e73921eacbdf020a61ff15f00a4c240cb38a121553ee04bd66e62b28425ff9bc50f4f5e15135166573ac0888332a01a0db1faa2 "$T" | sha512sum -c -
S=$PORTS_SRC/dbus-$V; [ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
meson_port "$S" "$PORTS_BUILD/dbus" --sysconfdir=/etc --localstatedir=/var \
    -Druntime_dir=/run -Dsession_socket_dir=/tmp -Dsystemd=disabled \
    -Dapparmor=disabled -Dselinux=disabled -Dlibaudit=disabled \
    -Dlaunchd=disabled -Dx11_autolaunch=disabled -Dtraditional_activation=false \
    -Dmodular_tests=disabled -Dinstalled_tests=false -Dintrusive_tests=false \
    -Ddoxygen_docs=disabled -Dducktype_docs=disabled -Dxml_docs=disabled \
    -Dqt_help=disabled -Depoll=enabled -Dinotify=enabled -Dkqueue=disabled
$DCC -O2 -o "$PREFIX_ROOT/usr/bin/dbustest" "$UL/ports/src/dbustest.c" \
    -I"$PREFIX_ROOT/usr/include/dbus-1.0" -I"$PREFIX_ROOT/usr/lib/dbus-1.0/include" \
    -L"$PREFIX_ROOT/usr/lib" -Wl,-rpath-link,"$PREFIX_ROOT/usr/lib" -ldbus-1
install -m755 "$UL/ports/scripts/dbussmoke" "$PREFIX_ROOT/usr/bin/dbussmoke"
echo "dbus $V + session bus regression installed (activation/systemd/X11 disabled)"
