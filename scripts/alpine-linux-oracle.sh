#!/bin/sh
# Run the SAME signed x86 desktop against Linux, not against the 9os kernel.
# Usage: sudo scripts/alpine-linux-oracle.sh [desktop-root] [native|callback] > log
# This is a disposable test-sandbox operation; mount/PID namespaces are required.
set -eu
SELF=$(realpath "$0")
INNER=0
if [ "${1:-}" = --inner ];then INNER=1;shift;fi
ROOT=$(realpath "${1:-$(dirname "$SELF")/../userland/alpine-desktop-root-x86_64}")
MODE=${2:-callback}
[ "$(uname -m)" = x86_64 ] || { echo 'Linux oracle is x86_64 only' >&2;exit 2; }
[ "$(id -u)" = 0 ] || { echo 'Run with sudo in a disposable test sandbox' >&2;exit 2; }
case "$MODE" in native|callback) ;; *) echo 'Expected native or callback' >&2;exit 2 ;; esac
python3 -c 'import json,sys;p=json.load(open(sys.argv[1]));assert p["arch"]=="x86_64" and p["profile"]=="desktop"' "$ROOT/etc/9os-alpine.json"
# build.py assembles signed packages; the small same-source consumers are
# installed separately by alpine-desktop-tests.sh. Reject incomplete fixtures
# before spending minutes waiting for impossible rendering/model gates.
for helper in desktopframecheck desktopa11y;do
 [ -x "$ROOT/usr/bin/$helper" ] || {
  echo "Missing desktop fixture helper: $helper (install from alpine-root-x86_64/usr/bin)" >&2
  exit 2
 }
done
if [ "$INNER" != 1 ];then
 exec timeout -k 10 480 unshare --mount --pid --fork "$SELF" --inner "$ROOT" "$MODE"
fi
mount --make-rprivate /
# No modifications to packaged files or the root used by a concurrent frozen VM.
mount --bind "$ROOT" "$ROOT"
mount -o remount,bind,ro "$ROOT"
mount -t proc proc "$ROOT/proc"
mount -t tmpfs -o mode=1777 tmpfs "$ROOT/tmp"
mount -t tmpfs -o mode=755 tmpfs "$ROOT/run"
# The VM init normally creates this home; Linux does not run the 9os init.
mount -t tmpfs -o mode=755 tmpfs "$ROOT/home"
mkdir -m 700 "$ROOT/home/user"
chown 1000:1000 "$ROOT/home/user"
mount -t tmpfs -o mode=755 tmpfs "$ROOT/dev"
for f in null zero random urandom;do
 touch "$ROOT/dev/$f"
 mount --bind "/dev/$f" "$ROOT/dev/$f"
done
mkdir -p "$ROOT/dev/shm" "$ROOT/dev/pts"
mount -t tmpfs -o mode=1777 tmpfs "$ROOT/dev/shm"
mount -t devpts -o newinstance,ptmxmode=0666,mode=0620 devpts "$ROOT/dev/pts"
ln -s pts/ptmx "$ROOT/dev/ptmx"
exec chroot --userspec=1000:1000 "$ROOT" /usr/bin/env -i \
 PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
 HOME=/home/user USER=user LOGNAME=user LANG=C.UTF-8 \
 ALPINE_PLASMA_OUTPUT=headless ALPINE_PLASMA_PRESENTATION="$MODE" \
 ALPINE_PLASMA_SERIAL_PNG=1 ALPINE_PLASMA_INTERACTIVE=1 \
 ALPINE_PLASMA_FILEMANAGER=1 ALPINE_DOLPHIN_HOLD=50 /bin/sh -c '
set -eu
cd /home/user
printf "LINUX_ORACLE_KERNEL=%s\n" "$(uname -s)"
printf "LINUX_ORACLE_UID=%s\n" "$(id -u)"
alpine-plasma-probe > /tmp/linux-plasma.log 2>&1 & session=$!
for i in $(seq 240);do
 grep -q DOLPHIN_MODEL_READY /tmp/linux-plasma.log && break
 kill -0 "$session" 2>/dev/null || break
 sleep 1
done
if grep -q DOLPHIN_MODEL_READY /tmp/linux-plasma.log;then
 display=
 for f in /tmp/alpine-plasma-*/wayland-*;do [ ! -S "$f" ] || display=$f;done
 [ -n "$display" ] || { cat /tmp/linux-plasma.log;exit 1; }
 export WAYLAND_DISPLAY="$display" XDG_RUNTIME_DIR="${display%/*}"
 deadline=$(( $(date +%s) + 35 ));n=0
 while [ "$(date +%s)" -lt "$deadline" ];do
  if grim /tmp/linux-frame.png;then
   echo LINUX_FRESH_PNG_BEGIN
   base64 /tmp/linux-frame.png
   echo LINUX_FRESH_PNG_END
   n=$((n+1))
  fi
  sleep 1
 done
 echo LINUX_FRESH_CAPTURE_COUNT=$n
fi
if wait "$session";then r=0;else r=$?;fi
cat /tmp/linux-plasma.log
exit "$r"
'
