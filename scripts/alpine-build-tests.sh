#!/bin/sh
# Build only our small regression consumer against signed PREBUILT musl headers/libs.
# Never build Qt, Mesa, LLVM, libc or BusyBox here. Non-x86 invocation is CI-only.
set -eu
cd "$(dirname "$0")/.."
ARCH=${1:-x86_64}
case "$ARCH" in x86_64|aarch64|riscv64) ;; *) echo 'Unsupported architecture' >&2;exit 1;; esac
S=$(cd "userland/alpine-root-$ARCH" && pwd -P)
for test in ptracestoptest fallocatetest unixcredtest procinfotest dircookietest proctasktest mountinfotest pimutextest ioctlwidthtest futexbitsettest futexrequeuetest eventfdwaketest alarmcputest pollwakebench desktopframecheck desktopa11y; do
clang --target="$ARCH-linux-musl" --sysroot="$S" -O2 -Wall -Wextra -fPIE -pie -fuse-ld=lld -nostdlib \
    "$S/usr/lib/Scrt1.o" "$S/usr/lib/crti.o" "userland/tests/$test.c" \
    -L"$S/usr/lib" -lc "$S/usr/lib/crtn.o" \
    -Wl,--dynamic-linker,/lib/ld-musl-$ARCH.so.1 -o "$S/usr/bin/$test"
done
