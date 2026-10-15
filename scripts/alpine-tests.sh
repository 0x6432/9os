#!/bin/sh
# No Qt compilation. Use source profile separately for its larger regression suite.
set -eu
ARCH=${1:-x86_64}
cd "$(dirname "$0")/.."
L=build/test-alpine-$ARCH.log
mkdir -p build
python3 scripts/qemu-test.py "$ARCH" --userland alpine --memory 2048M --boot-timeout 300 --timeout 90 --log "$L" \
    'test "$(cat /etc/alpine-release)" = 3.22.6 && uname -a && test ! -e /usr/lib/libc++.so' \
    'dbus-run-session -- dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ListNames' \
    'dbus-run-session -- sh -c "exit 23"; test $? = 23' \
    alpine-qtsmoke 'su user -s /bin/sh -c alpine-qtsmoke' \
    "host:python3 scripts/alpine-pixel-check.py $L" \
    'grep -q "^violations 0" /proc/lockdep'
