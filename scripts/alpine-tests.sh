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
    "timeout 30 eventfdwaketest" 'su user -s /bin/sh -c "timeout 30 eventfdwaketest"' \
    fallocatetest ptracestoptest unixcredtest procinfotest proctasktest mountinfotest pimutextest ioctlwidthtest "timeout 30 futexbitsettest" \
    'su user -s /bin/sh -c "timeout 30 futexbitsettest"' "timeout 30 futexrequeuetest" 'su user -s /bin/sh -c "timeout 30 futexrequeuetest"' "timeout 10 alarmcputest" \
    'su user -s /bin/sh -c "timeout 10 alarmcputest"' pollwakebench \
    'su user -s /bin/sh -c pollwakebench' \
    'dircookietest /proc/self /proc/self/fd /proc/self/task /sys/class' 'su user -s /bin/sh -c "dircookietest /proc/self /sys/class"' \
    'su user -s /bin/sh -c procinfotest' 'su user -s /bin/sh -c proctasktest' alpine-proctask-smoke 'su user -s /bin/sh -c alpine-proctask-smoke' \
    'pidof init && ps -o pid,ppid,comm && top -b -n 1 >/tmp/top.out && grep -q init /tmp/top.out' \
    'timeout -s KILL 20 strace -f -o /tmp/alpine-strace.log /bin/sh -c "echo native-trace-ok; /bin/true" && grep -q execve /tmp/alpine-strace.log' \
    alpine-qtsmoke 'su user -s /bin/sh -c alpine-qtsmoke' \
    "host:python3 scripts/alpine-pixel-check.py $L" \
    alpine-wayland-smoke 'ALPINE_WAYLAND_USER=user alpine-wayland-smoke' \
    "host:python3 scripts/alpine-wayland-check.py $L" \
    'grep -q " /run tmpfs " /proc/mounts' \
    'mountinfotest --mutate' 'su user -s /bin/sh -c mountinfotest' \
    'grep -q "^violations 0" /proc/lockdep'
