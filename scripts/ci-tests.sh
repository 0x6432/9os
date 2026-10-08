#!/bin/sh
# Run the in-tree regression tests in QEMU for ARCH (used by CI; works locally too).
# usage: scripts/ci-tests.sh ARCH [extra qemu-test.py options]
# Three boots: (1) the main suite on the initramfs root with an empty ext2 disk (vda) and a raw
# scratch disk (vdb); (2) root=/dev/vda1 on an ext2 image of the userland root, writing data
# that (3) a second disk-root boot verifies; e2fsck checks every image afterwards (M29/M30).
ARCH=${1:-x86_64}; shift
cd "$(dirname "$0")/.."
PATH=$PATH:/usr/sbin:/sbin
WL=; [ -e userland/build/ports-root-$ARCH/usr/bin/wltest ] && WL=wltest
B=build/ci-$ARCH; mkdir -p "$B"
sh scripts/mkdisk.sh "$B/ext2.img" 128 || exit 1
sh scripts/mkdisk.sh "$B/root.img" 256 "userland/root-$ARCH" || exit 1
rm -f "$B/scratch.img"; truncate -s 16M "$B/scratch.img"
fsck_img() {   # e2fsck the partition at 1 MiB
    dd if="$1" of="$1.part" bs=1M skip=1 status=none && e2fsck -fn "$1.part" >"$1.fsck" 2>&1
    r=$?; rm -f "$1.part"
    [ $r = 0 ] && echo "PASS   e2fsck $1" || { echo "FAIL   e2fsck $1"; cat "$1.fsck"; }
    return $r
}
rc=0
BIGMD5=$(python3 scripts/net-host-server.py --md5)
python3 scripts/qemu-test.py "$ARCH" --log "build/test-$ARCH.log" --disk "$B/ext2.img" --disk "$B/scratch.img" --net-test "$@" \
    libctest cowtest ipctest ptytest inotifytest dyntest mapprivtest smptest faulttest pcputest slabtest idletest pipetest futextest efdtest socktest polltest fdtest filetest vfstest vmtest timetest afftest nicetest balancetest hardentest irqtest permtest logintest \
    "bash -c 'a=(1 2 3); s=0; for i in \${a[@]}; do s=\$((s+i)); done; [ \$s = 6 ]'" \
    "echo hello | gzip | gunzip | grep -q hello" \
    ${WL} drmdemo \
    nettest net2test "ping -c 2 -W 2 127.0.0.1" \
    "for i in 1 2 3 4 5 6 7 8 9 10; do ifconfig eth0 | grep -q 'inet addr:10.0.2.15' && break; sleep 1; done; ifconfig eth0 | grep -q 'inet addr:10.0.2.15'" \
    "route -n | grep -q '^0.0.0.0 *10.0.2.2'" "grep -q nameserver /etc/resolv.conf" \
    "nettest -x 10.0.2.2 @HP@" \
    "wget -q -O - http://10.0.2.2:@HP@/hello | grep -qx 'hello from the host'" \
    "wget -q -O /root/big http://10.0.2.2:@HP@/big && md5sum /root/big | grep -q $BIGMD5 && rm /root/big" \
    "nettest -s 8080" \
    "host:python3 scripts/net-host-server.py --echo-check @FP@" \
    "blktest /dev/vdb" "ext2test /dev/vda1 /mnt" "blktest -w /dev/vdb 77" \
    "grep -q '^violations 0' /proc/lockdep" || rc=1
fsck_img "$B/ext2.img" || rc=1
python3 scripts/qemu-test.py "$ARCH" --log "build/test-diskroot-$ARCH.log" --disk "$B/root.img" --disk "$B/scratch.img" \
    --cmdline "root=/dev/vda1" "$@" \
    "grep -q '^/dev/vda1 / ext2 rw' /proc/mounts" "blktest -v /dev/vdb 77" \
    libctest dyntest mapprivtest filetest vfstest vmtest "permtest /tmp" logintest "mkdir /root/e2 && ext2test -d /root/e2 && rmdir /root/e2" \
    "cp -a /usr /root/usr2 && echo persist > /root/keep && sync" \
    "grep -q '^violations 0' /proc/lockdep" || rc=1
python3 scripts/qemu-test.py "$ARCH" --log "build/test-diskroot2-$ARCH.log" --disk "$B/root.img" \
    --cmdline "root=/dev/vda1" "$@" \
    "grep -qx persist /root/keep" "diff -r /usr /root/usr2" "rm -rf /root/usr2 /root/keep" || rc=1
if grep -aq "not cleanly unmounted" "build/test-diskroot2-$ARCH.log"; then echo "FAIL   clean shutdown of the disk root"; rc=1
else echo "PASS   clean shutdown of the disk root"; fi
fsck_img "$B/root.img" || rc=1
exit $rc
