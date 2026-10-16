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
# A directly invoked physical test may not have run the desktop suite first.
# Fail before packing/booting if its same-source consumers were not installed.
for helper in desktopframecheck desktopa11y;do
 [ -x "$ROOT/usr/bin/$helper" ] || {
  echo "Missing desktop fixture helper: $helper (run alpine-desktop-tests.sh or install its consumers)" >&2
  exit 2
 }
done
scripts/mkdisk.sh "$DISK" 3584 "$ROOT"
export QEMU_EXTRA="${QEMU_EXTRA:-} -monitor unix:$S,server,nowait"
python3 scripts/qemu-test.py "$ARCH" --userland alpine --memory 3072M \
 --disk "$DISK" --cmdline root=/dev/vda1 --boot-timeout 300 --timeout 780 --log "$L" \
 alpine-desktop-services \
 'su user -s /bin/sh -c alpine-udisks-smoke' \
 '(if ALPINE_PLASMA_OUTPUT=kms ALPINE_PLASMA_SERIAL_PNG=0 ALPINE_PLASMA_INTERACTIVE=1 ALPINE_PLASMA_FILEMANAGER=1 ALPINE_PLASMA_INPUT_DONE_FILE=/tmp/kms-input-done ALPINE_DOLPHIN_DONE_FILE=/tmp/dolphin-capture-done alpine-plasma-user-probe >/tmp/plasma-kms.log 2>&1;then echo 0;else echo $?;fi >/tmp/plasma-kms.status.tmp;mv /tmp/plasma-kms.status.tmp /tmp/plasma-kms.status) & echo $! >/tmp/plasma-kms.pid' \
 'for i in $(seq 700);do grep -q ALPINE_PLASMA_INPUT_READY /tmp/plasma-kms.log && break;kill -0 "$(cat /tmp/plasma-kms.pid)" 2>/dev/null || break;sleep 1;done;grep -q ALPINE_PLASMA_INPUT_READY /tmp/plasma-kms.log || { tail -n 160 /tmp/plasma-kms.log; false; }' \
 'test ! -e /tmp/kms-input-ok && test ! -e /tmp/kms-tty && test ! -e /tmp/kms-uid && test ! -e /tmp/kms-input-done && test ! -e /tmp/dolphin-capture-done' \
 "host:python3 scripts/qemu-screenshot.py $S $P-before.png" \
 "host:python3 scripts/qemu-type.py $S 'tty > /tmp/kms-tty; id -u > /tmp/kms-uid; echo kms-input-ok > /tmp/kms-input-ok.tmp; mv /tmp/kms-input-ok.tmp /tmp/kms-input-ok\n'" \
 'for i in $(seq 20);do test ! -e /tmp/kms-input-ok || break;sleep 1;done;cat /tmp/kms-tty;test "$(cat /tmp/kms-input-ok)" = kms-input-ok && grep -q "^/dev/pts/" /tmp/kms-tty && test "$(cat /tmp/kms-uid)" = 1000' \
 "host:python3 scripts/qemu-screenshot.py $S $P-after.png" \
 'test "$(cat /tmp/kms-input-ok)" = kms-input-ok && test "$(cat /tmp/kms-uid)" = 1000 && touch /tmp/kms-input-done' \
 'for i in $(seq 300);do grep -q DOLPHIN_MODEL_READY /tmp/plasma-kms.log && break;kill -0 "$(cat /tmp/plasma-kms.pid)" 2>/dev/null || break;sleep 1;done;grep -q "DOLPHIN_MODEL_READY uid=1000" /tmp/plasma-kms.log || { tail -n 100 /tmp/plasma-kms.log;false; }' \
 "host:python3 scripts/qemu-dolphin-capture.py $S $P-dolphin.png" \
 'grep -q "DOLPHIN_MODEL_READY uid=1000" /tmp/plasma-kms.log && touch /tmp/dolphin-capture-done' \
 'for i in $(seq 150);do test ! -f /tmp/plasma-kms.status || break;sleep 1;done;cat /tmp/plasma-kms.log;test "$(cat /tmp/plasma-kms.status)" = 0 && grep -qx "alpine-dolphin: PASSED real UID1000 directory listing, rename/create notifications and clean exit" /tmp/plasma-kms.log' \
 'grep -q "^violations 0" /proc/lockdep'
