#!/bin/sh
# Only x86_64 locally; other architectures are executed by PR CI.
# Physical key -> libinput -> Sway -> nested KWin -> genuine Konsole PTY.
set -eu
cd "$(dirname "$0")/.."
ARCH=${1:-x86_64}
ROOT=userland/alpine-desktop-root-$ARCH
DISK=build/alpine-desktop-$ARCH.img
L=build/test-alpine-kms-$ARCH.log
D=$(mktemp -d /tmp/9os-kms-XXXXXX)
trap 'rm -rf "$D"' EXIT HUP INT TERM
S=$D/monitor.sock
P=$(pwd -P)/build/test-alpine-kms-$ARCH
scripts/mkdisk.sh "$DISK" 3584 "$ROOT"
export QEMU_EXTRA="${QEMU_EXTRA:-} -monitor unix:$S,server,nowait"
python3 scripts/qemu-test.py "$ARCH" --userland alpine --memory 3072M \
 --disk "$DISK" --cmdline root=/dev/vda1 --boot-timeout 300 --timeout 780 --log "$L" \
 alpine-desktop-services \
 '(ALPINE_PLASMA_OUTPUT=kms ALPINE_PLASMA_INTERACTIVE=1 ALPINE_PLASMA_HOLD=90 alpine-plasma-user-probe >/tmp/plasma-kms.log 2>&1 & echo $! >/tmp/plasma-kms.pid)' \
 'for i in $(seq 700);do grep -q ALPINE_PLASMA_INPUT_READY /tmp/plasma-kms.log && break;kill -0 "$(cat /tmp/plasma-kms.pid)" 2>/dev/null || break;sleep 1;done;grep -q ALPINE_PLASMA_INPUT_READY /tmp/plasma-kms.log || { tail -n 160 /tmp/plasma-kms.log; false; }' \
 'test ! -e /tmp/kms-input-ok && test ! -e /tmp/kms-tty && test ! -e /tmp/kms-uid' \
 "host:python3 scripts/qemu-screenshot.py $S $P-before.png" \
 "host:python3 scripts/qemu-type.py $S 'tty > /tmp/kms-tty; id -u > /tmp/kms-uid; echo kms-input-ok > /tmp/kms-input-ok\n'" \
 'for i in $(seq 20);do test ! -e /tmp/kms-input-ok || break;sleep 1;done;cat /tmp/kms-tty;test "$(cat /tmp/kms-input-ok)" = kms-input-ok && grep -q "^/dev/pts/" /tmp/kms-tty && test "$(cat /tmp/kms-uid)" = 1000' \
 "host:python3 scripts/qemu-screenshot.py $S $P-after.png" \
 'for i in $(seq 150);do kill -0 "$(cat /tmp/plasma-kms.pid)" 2>/dev/null || break;sleep 1;done;cat /tmp/plasma-kms.log;! kill -0 "$(cat /tmp/plasma-kms.pid)" 2>/dev/null' \
 'grep -q "^violations 0" /proc/lockdep'
