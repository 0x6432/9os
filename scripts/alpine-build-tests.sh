#!/bin/sh
# Build only our small regression consumer against signed PREBUILT musl headers/libs.
# Never build Qt, Mesa, LLVM, libc or BusyBox here. Non-x86 invocation is CI-only.
set -eu
cd "$(dirname "$0")/.."
ARCH=${1:-x86_64}
case "$ARCH" in x86_64|aarch64|riscv64) ;; *) echo 'Unsupported architecture' >&2;exit 1;; esac
S=$(cd "userland/alpine-root-$ARCH" && pwd -P)
for test in ptracestoptest fallocatetest unixcredtest procinfotest dircookietest proctasktest mountinfotest pimutextest ioctlwidthtest futexbitsettest futexrequeuetest eventfdwaketest evdevframetest pollfiltertest alarmcputest pollwakebench desktopframecheck desktopa11y inputeventcapture; do
clang --target="$ARCH-linux-musl" --sysroot="$S" -O2 -Wall -Wextra -fPIE -pie -fuse-ld=lld -nostdlib \
    "$S/usr/lib/Scrt1.o" "$S/usr/lib/crti.o" "userland/tests/$test.c" \
    -L"$S/usr/lib" -lc "$S/usr/lib/crtn.o" \
    -Wl,--dynamic-linker,/lib/ld-musl-$ARCH.so.1 -o "$S/usr/bin/$test"
done
# Tiny, opt-in Qt6.8.3 platform-key ABI observer and independent C forwarding
# surrogate. This builds NO Qt library and links NO Qt C++ runtime.
mkdir -p "$S/usr/lib/9os-tests"
for model in observer mock;do
 case "$model" in
  observer) SRC=qt-key-observer;OUT="$S/usr/lib/lib9os-qt-key-observer.so";;
  mock) SRC=qt-key-observer-mock;OUT="$S/usr/lib/9os-tests/libqt-key-observer-mock.so";;
 esac
 clang --target="$ARCH-linux-musl" --sysroot="$S" -O2 -Wall -Wextra -Werror -D_FORTIFY_SOURCE=2 -shared -fPIC -fuse-ld=lld -nostdlib \
  "$S/usr/lib/crti.o" "userland/tests/$SRC.c" -L"$S/usr/lib" -lc "$S/usr/lib/crtn.o" \
  -Wl,--version-script="userland/tests/$SRC.map" -o "$OUT"
done
clang --target="$ARCH-linux-musl" --sysroot="$S" -O2 -Wall -Wextra -Werror -D_FORTIFY_SOURCE=2 -fPIE -pie -fuse-ld=lld -nostdlib \
 "$S/usr/lib/Scrt1.o" "$S/usr/lib/crti.o" userland/tests/qt-key-observer-caller.c \
 -L"$S/usr/lib/9os-tests" -lqt-key-observer-mock -L"$S/usr/lib" -lc "$S/usr/lib/crtn.o" \
 -Wl,-rpath,/usr/lib/9os-tests -Wl,--dynamic-linker,/lib/ld-musl-$ARCH.so.1 -o "$S/usr/bin/qt-key-observer-caller"
