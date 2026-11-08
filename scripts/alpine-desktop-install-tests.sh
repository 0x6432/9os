#!/bin/sh
# Copy already-built small consumers to the desktop root; never build, pack or boot.
set -eu
cd "$(dirname "$0")/.."
ARCH=${1:-x86_64}
case "$ARCH" in x86_64|aarch64|riscv64) ;; *) echo "Unsupported architecture" >&2;exit 1;; esac
ROOT=userland/alpine-desktop-root-$ARCH
for test in fallocatetest ptracestoptest unixcredtest procinfotest dircookietest proctasktest mountinfotest pimutextest ioctlwidthtest desktopframecheck desktopa11y desktopa11y-dbus trace-snapshot trace-snapshot-target inputeventcapture;do cp "userland/alpine-root-$ARCH/usr/bin/$test" "$ROOT/usr/bin/$test";done
mkdir -p "$ROOT/usr/lib/9os-tests"
cp "userland/alpine-root-$ARCH/usr/lib/lib9os-qt-key-observer.so" "$ROOT/usr/lib/"
cp "userland/alpine-root-$ARCH/usr/lib/9os-tests/libqt-key-observer-mock.so" "$ROOT/usr/lib/9os-tests/"
cp "userland/alpine-root-$ARCH/usr/bin/qt-key-observer-caller" "$ROOT/usr/bin/"
