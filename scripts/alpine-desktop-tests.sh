#!/bin/sh
# Only x86_64 locally; aarch64/riscv64 are invoked by PR CI.
# Signed prebuilt userland on disk; only our kernel/tiny ABI consumers are built.
set -eu
cd "$(dirname "$0")/.."
ARCH=${1:-x86_64}
ROOT=userland/alpine-desktop-root-$ARCH
DISK=build/alpine-desktop-$ARCH.img
L=build/test-alpine-desktop-$ARCH.log
for test in fallocatetest ptracestoptest;do cp "userland/alpine-root-$ARCH/usr/bin/$test" "$ROOT/usr/bin/$test";done
scripts/mkdisk.sh "$DISK" 3584 "$ROOT"
python3 scripts/qemu-test.py "$ARCH" --userland alpine --memory 3072M \
 --disk "$DISK" --cmdline root=/dev/vda1 --boot-timeout 300 --timeout 180 --log "$L" \
 'grep -q "root=/dev/vda1" /proc/cmdline' fallocatetest ptracestoptest \
 'ALPINE_WAYLAND_RENDERER=opengl alpine-wayland-smoke' \
 'ALPINE_WAYLAND_RENDERER=opengl ALPINE_WAYLAND_USER=user alpine-wayland-smoke' \
 "host:python3 scripts/alpine-wayland-check.py $L --renderer opengl" \
 alpine-kwin-probe alpine-plasma-probe \
 "host:python3 scripts/alpine-plasma-check.py $L" \
 'grep -q "^violations 0" /proc/lockdep'
