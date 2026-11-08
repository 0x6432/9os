#!/bin/sh
# Only x86_64 locally; other architectures are executed by PR CI.
# Physical key -> libinput -> Sway -> nested KWin -> genuine Konsole PTY.
set -eu
cd "$(dirname "$0")/.."
ARCH=${1:-x86_64}
case ${ALPINE_QT_KEY_TRACE:-0} in
 0) service_command=alpine-desktop-services ;;
 1) service_command='export ALPINE_QT_KEY_TRACE=1; alpine-desktop-services' ;;
 *) echo 'ALPINE_QT_KEY_TRACE must be 0 or 1' >&2;exit 2 ;;
esac
case ${ALPINE_INPUT_CHAIN_DIAGNOSTICS:-0} in
 0) input_chain_diagnostics=0 ;;
 1) input_chain_diagnostics=1;service_command="export ALPINE_INPUT_CHAIN_DIAGNOSTICS=1; echo INPUT_CHAIN_DIAGNOSTICS_ENABLED; $service_command" ;;
 *) echo 'ALPINE_INPUT_CHAIN_DIAGNOSTICS must be 0 or 1' >&2;exit 2 ;;
esac
case ${ALPINE_A11Y_DBUS_VALIDATE:-0} in
 0) ;;
 1) service_command="export ALPINE_A11Y_DBUS_VALIDATE=1; $service_command" ;;
 *) echo 'ALPINE_A11Y_DBUS_VALIDATE must be 0 or 1' >&2;exit 2 ;;
esac
case ${ALPINE_TERMINAL_QUERY:-atspi} in
 atspi) ;;
 live-dbus) service_command="export ALPINE_TERMINAL_QUERY=live-dbus; $service_command" ;;
 *) echo 'ALPINE_TERMINAL_QUERY must be atspi or live-dbus' >&2;exit 2 ;;
esac
ROOT=userland/alpine-desktop-root-$ARCH
DISK=build/alpine-desktop-$ARCH.img
L=build/test-alpine-kms-$ARCH.log
D=$(mktemp -d /tmp/9os-kms-XXXXXX)
trap 'rm -rf "$D"' EXIT HUP INT TERM
S=$D/monitor.sock
Q=$D/qmp.sock
P=$(pwd -P)/build/test-alpine-kms-$ARCH
# A directly invoked physical test may not have run the desktop suite first.
# Fail before packing/booting if its same-source consumers were not installed.
for helper in desktopframecheck desktopa11y desktopa11y-dbus;do
 [ -x "$ROOT/usr/bin/$helper" ] || {
  echo "Missing desktop fixture helper: $helper (run alpine-desktop-tests.sh or install its consumers)" >&2
  exit 2
 }
done
if [ "$input_chain_diagnostics" = 1 ];then
 for helper in input-capture-session inputeventcapture input-capture-ready konsole-key-trace wayland-presentation-proxy;do
  [ -x "$ROOT/usr/bin/$helper" ] || {
   echo "Missing input-chain diagnostic helper: $helper (install the existing diagnostic consumers first)" >&2
   exit 2
  }
 done
 [ -r "$ROOT/usr/lib/lib9os-qt-key-observer.so" ] || {
  echo 'Missing input-chain diagnostic Qt observer library (install the existing observer first)' >&2
  exit 2
 }
fi
if [ "$input_chain_diagnostics" = 1 ];then
 run_id=$(date +%Y%m%d%H%M%S)-$$
 L=build/test-alpine-kms-$ARCH-input-chain-$run_id.log
 P=$(pwd -P)/build/test-alpine-kms-$ARCH-input-chain-$run_id
fi
# The experimental frozen route never creates a disk or invokes make. The
# caller supplies a fresh disposable disk copy and a separately reviewed manifest.
frozen=0
if [ -n "${M40_FROZEN_MANIFEST:-}" ] || [ -n "${M40_FROZEN_DISK:-}" ];then
 [ -n "${M40_FROZEN_MANIFEST:-}" ] && [ -n "${M40_FROZEN_DISK:-}" ] || { echo 'Frozen manifest and disposable disk are both required' >&2;exit 2; }
 [ "$ARCH" = x86_64 ] && [ "$input_chain_diagnostics" = 1 ] || { echo 'Frozen mode requires x86_64 input-chain diagnostics' >&2;exit 2; }
 [ -z "${QEMU_EXTRA:-}" ] || { echo 'Frozen mode refuses QEMU_EXTRA overrides' >&2;exit 2; }
 DISK=$M40_FROZEN_DISK
 frozen=1
else
 scripts/mkdisk.sh "$DISK" 3584 "$ROOT"
 export QEMU_EXTRA="${QEMU_EXTRA:-} -monitor unix:$S,server,nowait -qmp unix:$Q,server,nowait"
fi
run_qemu_test() {
 if [ "$frozen" = 1 ];then
  python3 scripts/qemu-test.py "$ARCH" --userland alpine --memory 3072M \
   --disk "$DISK" --cmdline root=/dev/vda1 --boot-timeout 300 --timeout 780 --log "$L" \
   --frozen-manifest "$M40_FROZEN_MANIFEST" --monitor-socket "$S" --qmp-socket "$Q" "$@"
 else
  python3 scripts/qemu-test.py "$ARCH" --userland alpine --memory 3072M \
   --disk "$DISK" --cmdline root=/dev/vda1 --boot-timeout 300 --timeout 780 --log "$L" "$@"
 fi
}
set -- "$service_command" \
 'su user -s /bin/sh -c alpine-udisks-smoke' \
 '(if ALPINE_PLASMA_OUTPUT=kms ALPINE_PLASMA_SERIAL_PNG=0 ALPINE_PLASMA_INTERACTIVE=1 ALPINE_PLASMA_FILEMANAGER=1 ALPINE_PLASMA_INPUT_DONE_FILE=/tmp/kms-input-done ALPINE_DOLPHIN_DONE_FILE=/tmp/dolphin-capture-done alpine-plasma-user-probe >/tmp/plasma-kms.log 2>&1;then echo 0;else echo $?;fi >/tmp/plasma-kms.status.tmp;mv /tmp/plasma-kms.status.tmp /tmp/plasma-kms.status) & echo $! >/tmp/plasma-kms.pid' \
 'for i in $(seq 700);do grep -qx ALPINE_PLASMA_INPUT_READY /tmp/plasma-kms.log && break;kill -0 "$(cat /tmp/plasma-kms.pid)" 2>/dev/null || break;sleep 1;done;grep -x KONSOLE_FOCUSED_TERMINAL_READY /tmp/plasma-kms.log && grep -x ALPINE_PLASMA_INPUT_READY /tmp/plasma-kms.log || { tail -n 160 /tmp/plasma-kms.log; false; }'
input_type_step="host:python3 scripts/qemu-ready-type.py --log $L --ready-step 3 --qmp $Q 'tty > /tmp/kms-tty; id -u > /tmp/kms-uid; echo kms-input-ok > /tmp/kms-input-ok.tmp; mv /tmp/kms-input-ok.tmp /tmp/kms-input-ok\n'"
if [ "$input_chain_diagnostics" = 1 ];then
 input_type_step="host:python3 scripts/qemu-ready-type.py --require-input-capture-ready --log $L --ready-step 3 --qmp $Q 'tty > /tmp/kms-tty; id -u > /tmp/kms-uid; echo kms-input-ok > /tmp/kms-input-ok.tmp; mv /tmp/kms-input-ok.tmp /tmp/kms-input-ok\n'"
fi
set -- "$@" \
 'test ! -e /tmp/kms-input-ok && test ! -e /tmp/kms-tty && test ! -e /tmp/kms-uid && test ! -e /tmp/kms-input-done && test ! -e /tmp/dolphin-capture-done' \
 "host:python3 scripts/qemu-screenshot.py $S $P-before.png"
if [ "$input_chain_diagnostics" = 1 ];then
 # Start after the pre-type screenshot and gate immediately before typing.
 set -- "$@" 'if grep -qx KONSOLE_FOCUSED_TERMINAL_READY /tmp/plasma-kms.log && grep -qx ALPINE_PLASMA_INPUT_READY /tmp/plasma-kms.log; then if [ -e /tmp/kms-input-chain.raw.log ] || [ -e /tmp/kms-input-chain.raw.status ] || [ -e /tmp/kms-input-chain.raw.pid ]; then echo INPUT_CHAIN_CAPTURE_SKIPPED stale_state; else input-capture-session /tmp/kms-input-chain.raw.log /tmp/kms-input-chain.raw.status 45 >/tmp/kms-input-chain.launch.log 2>&1 & echo "$!" >/tmp/kms-input-chain.raw.pid; echo INPUT_CHAIN_CAPTURE_STARTED seconds=45; fi; else echo INPUT_CHAIN_CAPTURE_SKIPPED readiness_not_established; fi'
 capture_ready_step=$#
 set -- "$@" 'input-capture-ready /tmp/kms-input-chain.raw.log 10 /tmp/kms-input-chain.raw.status'
 input_type_step="host:python3 scripts/qemu-ready-type.py --require-input-capture-ready --capture-ready-step $capture_ready_step --log $L --ready-step 3 --qmp $Q 'tty > /tmp/kms-tty; id -u > /tmp/kms-uid; echo kms-input-ok > /tmp/kms-input-ok.tmp; mv /tmp/kms-input-ok.tmp /tmp/kms-input-ok\n'"
fi
set -- "$@" \
 "$input_type_step" \
 'for i in $(seq 20);do test ! -e /tmp/kms-input-ok || break;sleep 1;done;cat /tmp/kms-tty;test "$(cat /tmp/kms-input-ok)" = kms-input-ok && grep -q "^/dev/pts/" /tmp/kms-tty && test "$(cat /tmp/kms-uid)" = 1000' \
 "host:python3 scripts/qemu-screenshot.py $S $P-after.png" \
 'test "$(cat /tmp/kms-input-ok)" = kms-input-ok && test "$(cat /tmp/kms-uid)" = 1000 && touch /tmp/kms-input-done' \
 'for i in $(seq 300);do grep -Eq "^DOLPHIN_MODEL_READY uid=1000 service=[^ ]+ object=/[^ ]*$" /tmp/plasma-kms.log && break;kill -0 "$(cat /tmp/plasma-kms.pid)" 2>/dev/null || break;sleep 1;done;grep -Eq "^DOLPHIN_MODEL_READY uid=1000 service=[^ ]+ object=/[^ ]*$" /tmp/plasma-kms.log || { tail -n 100 /tmp/plasma-kms.log;false; }' \
 "host:python3 scripts/qemu-dolphin-capture.py $S $P-dolphin.png" \
 'grep -Eq "^DOLPHIN_MODEL_READY uid=1000 service=[^ ]+ object=/[^ ]*$" /tmp/plasma-kms.log && touch /tmp/dolphin-capture-done' \
 'for i in $(seq 150);do test ! -f /tmp/plasma-kms.status || break;sleep 1;done;cat /tmp/plasma-kms.log;test "$(cat /tmp/plasma-kms.status)" = 0 && grep -qx "alpine-dolphin: PASSED real UID1000 directory listing, rename/create notifications and clean exit" /tmp/plasma-kms.log' \
 'grep -q "^violations 0" /proc/lockdep'
if [ "$input_chain_diagnostics" = 1 ];then
 set -- "$@" 'if [ -f /tmp/kms-input-chain.raw.pid ]; then for i in $(seq 600); do [ -f /tmp/kms-input-chain.raw.status ] && break; sleep .1; done; echo INPUT_CHAIN_RAW_CAPTURE_BEGIN; if [ -f /tmp/kms-input-chain.raw.status ]; then echo "INPUT_CHAIN_RAW_STATUS=$(cat /tmp/kms-input-chain.raw.status)"; else echo INPUT_CHAIN_RAW_STATUS=missing; fi; if [ -f /tmp/kms-input-chain.raw.log ]; then cat /tmp/kms-input-chain.raw.log; else echo INPUT_CHAIN_RAW_LOG_MISSING; fi; if [ -f /tmp/kms-input-chain.launch.log ]; then echo INPUT_CHAIN_LAUNCH_LOG_BEGIN; cat /tmp/kms-input-chain.launch.log; echo INPUT_CHAIN_LAUNCH_LOG_END; fi; echo INPUT_CHAIN_RAW_CAPTURE_END; else echo INPUT_CHAIN_RAW_CAPTURE_NOT_STARTED; fi'
fi
if [ "$input_chain_diagnostics" = 1 ];then
 # Preserve the acceptance result independently; diagnostic failure cannot
 # turn an original failed gate into success.
 test_rc=0
 if run_qemu_test "$@";then
  test_rc=0
 else
  test_rc=$?
 fi
 diag_rc=0
 if python3 scripts/input-chain-check.py "$L" >"$D/input-chain-check.log" 2>&1;then
  diag_rc=0
 else
  diag_rc=$?
 fi
 cat "$D/input-chain-check.log"
 cat "$D/input-chain-check.log" >>"$L"
 printf 'INPUT_CHAIN_CHECK_RC=%s\n' "$diag_rc" >>"$L"
 if [ "$test_rc" -ne 0 ];then exit "$test_rc";fi
 exit "$diag_rc"
fi
run_qemu_test "$@"
