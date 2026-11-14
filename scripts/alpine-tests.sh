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
    "timeout 30 eventfdwaketest" 'su user -s /bin/sh -c "timeout 30 eventfdwaketest"' "timeout 35 pollfiltertest" 'su user -s /bin/sh -c "timeout 35 pollfiltertest"' "timeout 10 epollabitest" 'su user -s /bin/sh -c "timeout 10 epollabitest"' "timeout 10 epollrestarttest" 'su user -s /bin/sh -c "timeout 10 epollrestarttest"' "timeout 45 unixpolltest" 'su user -s /bin/sh -c "timeout 45 unixpolltest"' \
    "timeout 35 evdevframetest" \
    fallocatetest ptracestoptest unixcredtest procinfotest proctasktest mountinfotest pimutextest ioctlwidthtest "timeout 30 futexbitsettest" \
    'su user -s /bin/sh -c "timeout 30 futexbitsettest"' "timeout 30 futexrequeuetest" 'su user -s /bin/sh -c "timeout 30 futexrequeuetest"' "timeout 10 alarmcputest" \
    'su user -s /bin/sh -c "timeout 10 alarmcputest"' pollwakebench "pollwakebench --inotify" "pollwakebench --unix" "pollwakebench --unix-inotify" "pollwakebench --epoll" "pollwakebench --epoll-inotify" "pollwakebench --epoll-unix" "pollwakebench --epoll-unix-inotify" 'su user -s /bin/sh -c "pollwakebench --epoll"' 'su user -s /bin/sh -c "pollwakebench --epoll-inotify"' 'su user -s /bin/sh -c "pollwakebench --epoll-unix"' 'su user -s /bin/sh -c "pollwakebench --epoll-unix-inotify"' 'su user -s /bin/sh -c "pollwakebench --unix"' 'su user -s /bin/sh -c "pollwakebench --unix-inotify"' 'su user -s /bin/sh -c "pollwakebench --inotify"' \
    'su user -s /bin/sh -c pollwakebench' \
    'dircookietest /proc/self /proc/self/fd /proc/self/task /sys/class' 'su user -s /bin/sh -c "dircookietest /proc/self /sys/class"' \
    'su user -s /bin/sh -c procinfotest' 'su user -s /bin/sh -c proctasktest' alpine-proctask-smoke 'su user -s /bin/sh -c alpine-proctask-smoke' \
    'pidof init && ps -o pid,ppid,comm && top -b -n 1 >/tmp/top.out && grep -q init /tmp/top.out' \
    '(if timeout -s KILL 20 strace -f -o /tmp/alpine-strace.log /bin/sh -c "echo native-trace-ok; /bin/true" && grep -q execve /tmp/alpine-strace.log; then r=0; else r=$?; fi; [ "$r" -eq 0 ] || (d(){ [ -f /tmp/alpine-strace.log ] || return 1; t=$(mktemp) || return 1; trap '"'"'rm -f "$t" >/dev/null 2>&1 || :'"'"' EXIT; tail -c1025 /tmp/alpine-strace.log >"$t" || return 1; n=$(wc -c <"$t") || return 1; case $n in '"'"''"'"'|*[!0-9]*|0?*) return 1;; esac; [ "${#n}" -le 4 ] || return 1; [ "$n" -le 1025 ] || return 1; s=0; [ "$n" -le 1024 ] || s=1; n=$((n-s)); h=$(LC_ALL=C od -An -v -tx1 -j "$s" "$t") || return 1; printf %s "$h" >"$t" || return 1; h=$(LC_ALL=C tr -d '"'"'[:space:]'"'"' <"$t") || return 1; case $h in *[!0-9a-f]*) return 1;; esac; [ "${#h}" -eq "$((n*2))" ] || return 1; rm -f "$t" || return 1; printf '"'"'ALPINE_STRACE_TAIL bytes=%s truncated=%s hex=%s\n'"'"' "$n" "$s" "$h"; }; d || printf '"'"'ALPINE_STRACE_TAIL unknown\n'"'"' ) 2>/dev/null; exit "$r")' \
    alpine-qtsmoke 'su user -s /bin/sh -c alpine-qtsmoke' \
    "host:python3 scripts/alpine-pixel-check.py $L" \
    alpine-wayland-smoke 'ALPINE_WAYLAND_USER=user alpine-wayland-smoke' \
    "host:python3 scripts/alpine-wayland-check.py $L" \
    'grep -q " /run tmpfs " /proc/mounts' \
    'mountinfotest --mutate' 'su user -s /bin/sh -c mountinfotest' \
    'grep -q "^violations 0" /proc/lockdep'
