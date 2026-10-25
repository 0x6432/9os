#!/bin/sh
# Boot the exact tiny guest consumer on a signed PREBUILT Alpine Linux kernel.
# x86_64 only. No Qt/Mesa/LLVM/libc build, host input device or host root needed.
set -eu
cd "$(dirname "$0")/.."
[ "$(uname -m)" = x86_64 ] || { echo 'Linux input oracle requires x86_64 host' >&2;exit 2; }
S=$(cd userland/alpine-root-x86_64 && pwd -P)
[ -x "$S/usr/bin/evdevframetest" ] || { echo 'Run alpine-build-tests.sh x86_64 first' >&2;exit 2; }
L=${1:-build/evdev-linux-oracle.log}
mkdir -p "$(dirname "$L")"
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT HUP INT TERM
mkdir -p "$D/apk" "$D/init"/bin "$D/init"/lib "$D/init"/dev "$D/init"/proc "$D/init"/sys "$D/init"/modules
# Explicit NEW114 reference; all signatures/closure checks precede offline installation.
python3 scripts/evdev-linux-oracle-packages.py --target "$D/apk" \
 --manifest "$L.packages.lock.json" > "$L.packages.log" 2>&1
cp "$S/bin/busybox" "$D/init/bin/"
cp "$S/usr/bin/evdevframetest" "$D/init/bin/"
cp -L "$S/lib/ld-musl-x86_64.so.1" "$D/init/lib/ld-musl-x86_64.so.1"
for n in sh mount insmod poweroff;do ln -s busybox "$D/init/bin/$n";done
for n in evdev misc/uinput;do gzip -dc "$D/apk/lib/modules/6.12.114-0-virt/kernel/drivers/input/$n.ko.gz" > "$D/init/modules/$(basename "$n").ko";done
cat > "$D/init/init" <<'INIT'
#!/bin/sh
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
insmod /modules/evdev.ko
insmod /modules/uinput.ko
/bin/evdevframetest
rc=$?
echo LINUX_INPUT_ORACLE_RC=$rc
poweroff -f
INIT
chmod +x "$D/init/init"
(cd "$D/init" && find . -print0 | cpio -0 -o -H newc --quiet | gzip -1 > "$D/init.cpio.gz")
sha256sum "$D/apk/boot/vmlinuz-virt" "$D/init/bin/evdevframetest" "$D/init.cpio.gz" > "$L.fixtures.sha256"
timeout 90 qemu-system-x86_64 -machine q35,accel=tcg -cpu max -smp 2 -m 512M \
 -kernel "$D/apk/boot/vmlinuz-virt" -initrd "$D/init.cpio.gz" \
 -append 'console=ttyS0 panic=-1 rdinit=/init' -display none -serial stdio -no-reboot > "$L.raw" 2>&1
tr -d '\r' < "$L.raw" > "$L"
grep -E '^(evdevframetest:|LINUX_INPUT_ORACLE_RC=)' "$L"
grep -qx LINUX_INPUT_ORACLE_RC=0 "$L"
