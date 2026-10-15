#!/bin/sh
# Run the in-tree regression tests in QEMU for ARCH (used by CI; works locally too).
# usage: scripts/ci-tests.sh ARCH [extra qemu-test.py options]
# Three boots: (1) the main suite on the initramfs root with an empty ext2 disk (vda) and a raw
# scratch disk (vdb); (2) root=/dev/vda1 on an ext2 image of the userland root, writing data
# that (3) a second disk-root boot verifies; e2fsck checks every image afterwards (M29/M30).
ARCH=${1:-x86_64}; shift
cd "$(dirname "$0")/.."
PATH=$PATH:/usr/sbin:/sbin
EGLDMA=; GLSMOKE=;
# Keep a crashed smoke shell from hiding the first compositor/allocator failure.
# Preserve its status; diagnostics are not a retry or a substitute for passing frames.
GLDIAG='r=$?; echo "graphics smoke failure: status $r"; cat /proc/meminfo; ps; for d in /tmp/glsmoke-*; do [ -d "$d" ] || continue; for f in config client.log sway.log; do [ ! -f "$d/$f" ] || { echo "=== $d/$f ==="; cat "$d/$f"; }; done; done; (exit "$r")'
[ -e userland/build/ports-root-$ARCH/usr/bin/glsmoke ] && GLSMOKE="glsmoke || { $GLDIAG; }"
[ -e userland/build/ports-root-$ARCH/usr/bin/egltest ] && EGLDMA="egltest dmabuf"
EGL=; EGLGBM=; [ -e userland/build/ports-root-$ARCH/usr/bin/egltest ] && EGL=egltest && EGLGBM="egltest gbm"
JIT=; LLEGL=; LLEGLGBM=; LLEGLDMA=; LLGLSMOKE=
[ -e userland/build/ports-root-$ARCH/usr/bin/jittest ] && JIT=jittest
if [ -n "$JIT" ] && [ -n "$EGL" ]; then
    LLEGL="GALLIUM_DRIVER=llvmpipe LP_NUM_THREADS=2 egltest"
    LLEGLGBM="$LLEGL gbm"; LLEGLDMA="$LLEGL dmabuf"
    [ -n "$GLSMOKE" ] && LLGLSMOKE="GALLIUM_DRIVER=llvmpipe LP_NUM_THREADS=2 glsmoke"
fi
DBUS=; DBUSCLI=; DBUSUSER=; DBUSCLEAN=
if [ -e userland/build/ports-root-$ARCH/usr/bin/dbussmoke ]; then
    DBUS="for i in 1 2 3; do dbussmoke || exit 1; done"
    DBUSCLI="dbus-run-session -- sh -c 'dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ListNames | grep -q org.freedesktop.DBus'; r=\$?; [ \$r = 0 ] && { dbus-run-session -- sh -c 'exit 23'; [ \$? = 23 ]; }"
    DBUSUSER="mkdir -p /tmp/dbus-user && chown 1000:1000 /tmp/dbus-user && chmod 700 /tmp/dbus-user && su user -s /bin/sh -c 'XDG_RUNTIME_DIR=/tmp/dbus-user dbus-run-session -- dbustest'; r=\$?; rm -rf /tmp/dbus-user; [ \$r = 0 ]"
    # Upstream dbus-run-session sends SIGTERM then exits without waiting for the daemon.
    DBUSCLEAN="for i in 1 2 3 4 5 6 7 8 9 10; do pidof dbus-daemon >/dev/null || break; sleep 1; done; ! pidof dbus-daemon && [ \$(wc -c < /etc/machine-id) = 33 ]"
fi
QT=; QTUSER=; QTCLEAN=
if [ -e userland/build/ports-root-$ARCH/usr/bin/qtsmoke ]; then
    QT="qtsmoke && qtsmoke"
    QTUSER="mkdir -p /tmp/qt-user && chown 1000:1000 /tmp/qt-user && chmod 700 /tmp/qt-user && su user -s /bin/sh -c 'XDG_RUNTIME_DIR=/tmp/qt-user dbus-run-session -- qttest'; r=\$?; rm -rf /tmp/qt-user; [ \$r = 0 ]"
    QTCLEAN="$DBUSCLEAN"
fi
QML=; QMLUSER=
if [ -e userland/build/ports-root-$ARCH/usr/bin/qmlsmoke ]; then
    QML="qmlsmoke && qmlsmoke"
    QMLUSER="su user -s /bin/sh -c qmlsmoke"
fi
QTGUI=; QTGUIUSER=
if [ -e userland/build/ports-root-$ARCH/usr/bin/qtguismoke ]; then
    QTGUI="qtguismoke && qtguismoke"
    QTGUIUSER="su user -s /bin/sh -c qtguismoke"
fi
QTWAYLAND=; QTWAYLANDUSER=
if [ -e userland/build/ports-root-$ARCH/usr/bin/qtwaylandsmoke ]; then
    QTWAYLAND=qtwaylandsmoke
    QTWAYLANDUSER="QTWAYLAND_CLIENT_USER=user qtwaylandsmoke"
fi
QTGL=; QTGLUSER=; QTLLGL=; QTLLGLUSER=
if [ -e userland/build/ports-root-$ARCH/usr/bin/qtglsmoke ]; then
    QTGL=qtglsmoke
    QTGLUSER="QTWAYLAND_CLIENT_USER=user qtglsmoke"
    if [ -n "$JIT" ]; then
        QTLLGL="GALLIUM_DRIVER=llvmpipe LP_NUM_THREADS=2 qtglsmoke"
        QTLLGLUSER="GALLIUM_DRIVER=llvmpipe LP_NUM_THREADS=2 QTWAYLAND_CLIENT_USER=user qtglsmoke"
    fi
fi
CXX=; [ -e userland/build/ports-root-$ARCH/usr/bin/cxxtest ] && CXX=cxxtest
[ -e userland/build/ports-root-$ARCH/usr/bin/cxxtest-noeh ] && CXX="$CXX cxxtest-noeh"
WL=; [ -e userland/build/ports-root-$ARCH/usr/bin/wltest ] && WL=wltest
IN=; [ -e userland/build/ports-root-$ARCH/usr/bin/inputtest ] && IN=inputtest
KMS2=; [ -e userland/build/ports-root-$ARCH/usr/bin/kmstest ] && IN="$IN kmstest" && KMS2="KMSTEST_FLIP_DELAY_US=50000 kmstest"
[ -e userland/build/ports-root-$ARCH/usr/libexec/libinput/libinput-test-utils ] && IN="$IN /usr/libexec/libinput/libinput-test-utils"
# M37: sway on the KMS device, a foot client mapped (checked over sway IPC), clean exit
SW1=; SW2=; [ -e userland/build/ports-root-$ARCH/usr/bin/sway ] && SW1="fc-list | grep -q DejaVuSansMono.ttf" && SW2="(mkdir -p /tmp/xdg && chmod 700 /tmp/xdg && export XDG_RUNTIME_DIR=/tmp/xdg LIBSEAT_BACKEND=builtin LANG=C.UTF-8 WLR_RENDERER=pixman && (sway > /tmp/sway.log 2>&1 &) && for i in \$(seq 60); do ls /tmp/xdg/sway-ipc.* >/dev/null 2>&1 && break; sleep 1; done && export SWAYSOCK=\$(ls /tmp/xdg/sway-ipc.*) && swaymsg -q exec 'foot -a smoke' && for i in \$(seq 60); do swaymsg -t get_tree | grep -q '\"app_id\": \"smoke\"' && break; sleep 1; done; swaymsg -t get_tree | grep -q '\"app_id\": \"smoke\"'; r=\$?; swaymsg -q exit; for i in \$(seq 20); do pidof sway >/dev/null || break; sleep 1; done; ! pidof sway >/dev/null && [ \$r = 0 ] || { tail -20 /tmp/sway.log; false; })"
B=build/ci-$ARCH; mkdir -p "$B"
INODE_SIZE=256 sh scripts/mkdisk.sh "$B/ext2.img" 128 || exit 1
# Disk-root persistence deliberately keeps a second complete /usr copy. Size its
# storage from the actual payload plus filesystem/test headroom, not a fixed
# 256 MiB that can no longer hold both copies once Qt Gui is present.
ROOT_KIB=$(du -sk "userland/root-$ARCH" | awk '{print $1}')
USR_KIB=$(du -sk "userland/root-$ARCH/usr" | awk '{print $1}')
ROOT_MB=$(( (ROOT_KIB + USR_KIB + 1023) / 1024 + 64 ))
[ "$ROOT_MB" -ge 256 ] || ROOT_MB=256
echo "Disk-root validation image: $ROOT_MB MiB (root $ROOT_KIB KiB, /usr copy $USR_KIB KiB)"
sh scripts/mkdisk.sh "$B/root.img" "$ROOT_MB" "userland/root-$ARCH" || exit 1
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
    libctest cowtest vforktest orphantest ipctest ptytest inotifytest dyntest mapprivtest smptest faulttest pcputest slabtest idletest pipetest futextest futexoptest efdtest socktest polltest fdtest filetest vfstest vmtest timetest afftest nicetest balancetest hardentest irqtest permtest logintest ptracetest fallocatetest ptracestoptest unixcredtest procinfotest pimutextest locktest leasetest asynctest xattrtest acltest sigqtest sysfstest "sysfstest -u" uinputtest vttest looptest sndtest \
    "udevshim -c && grep -q E:ID_INPUT_KEYBOARD=1 \$(grep -l ID_INPUT_KEY= /run/udev/data/c13:*) && udevshim -q /dev/input/event0 | grep -qx ID_INPUT=1" \
    "bash -c 'a=(1 2 3); s=0; for i in \${a[@]}; do s=\$((s+i)); done; [ \$s = 6 ]'" \
    "echo hello | gzip | gunzip | grep -q hello" \
    "touch /tmp/fa && setfacl -m u:1000:r,g:2000:rw /tmp/fa && getfacl -n /tmp/fa | grep -q '^user:1000:r--' && getfacl -c /tmp/fa | grep -q '^mask::rw-' && setfacl -x u:1000 /tmp/fa && setfacl -b /tmp/fa && ! getfacl -c /tmp/fa | grep -q mask && mkdir /tmp/fd && setfacl -d -m u:1000:rwx /tmp/fd && getfacl -cdn /tmp/fd | grep -q '^user:1000:rwx' && touch /tmp/fd/x && getfacl -cn /tmp/fd/x | grep -q '^user:1000:rwx' && rm -rf /tmp/fa /tmp/fd" \
    ${DBUS:+"$DBUS"} ${DBUSCLI:+"$DBUSCLI"} ${DBUSUSER:+"$DBUSUSER"} ${DBUSCLEAN:+"$DBUSCLEAN"} ${QT:+"$QT"} ${QTUSER:+"$QTUSER"} ${QTCLEAN:+"$QTCLEAN"} ${QML:+"$QML"} ${QMLUSER:+"$QMLUSER"} ${QTGUI:+"$QTGUI"} ${QTGUIUSER:+"$QTGUIUSER"} ${CXX} ${JIT} ${LLEGL:+"$LLEGL"} ${LLEGLGBM:+"$LLEGLGBM"} ${LLEGLDMA:+"$LLEGLDMA"} ${EGL} ${EGLGBM:+"$EGLGBM"} ${EGLDMA:+"$EGLDMA"} ${WL} ${IN} ${KMS2:+"$KMS2"} \
    "T=/usr/libexec/libinput/libinput-test-suite; [ ! -x \$T ] || { udevshim -d; chvt 2; \$T -j1 --filter-group=udev > /tmp/li.log 2>&1; r=\$?; chvt 1; kill \$(cat /run/udev/udevshim.pid); [ \$r = 0 ] || grep -A12 'failed:' /tmp/li.log; [ \$r = 0 ]; }" \
    drmdemo ${SW1:+"$SW1"} ${SW2:+"$SW2"} ${GLSMOKE:+"$GLSMOKE"} ${LLGLSMOKE:+"$LLGLSMOKE"} ${QTWAYLAND:+"$QTWAYLAND"} ${QTWAYLANDUSER:+"$QTWAYLANDUSER"} ${QTGL:+"$QTGL"} ${QTGLUSER:+"$QTGLUSER"} ${QTLLGL:+"$QTLLGL"} ${QTLLGLUSER:+"$QTLLGLUSER"} \
    nettest net2test "ping -c 2 -W 2 127.0.0.1" \
    "for i in 1 2 3 4 5 6 7 8 9 10; do ifconfig eth0 | grep -q 'inet addr:10.0.2.15' && break; sleep 1; done; ifconfig eth0 | grep -q 'inet addr:10.0.2.15'" \
    "route -n | grep -q '^0.0.0.0 *10.0.2.2'" "grep -q nameserver /etc/resolv.conf" \
    "nettest -x 10.0.2.2 @HP@" \
    "wget -q -O - http://10.0.2.2:@HP@/hello | grep -qx 'hello from the host'" \
    "wget -q -O /root/big http://10.0.2.2:@HP@/big && md5sum /root/big | grep -q $BIGMD5 && rm /root/big" \
    "for i in 1 2 3 4 5 6 7 8 9 10; do ip -6 addr show eth0 | grep -q 'inet6 fec0::' && break; sleep 1; done; ip -6 addr show eth0 | grep -q 'inet6 fec0::.*/64 scope site dynamic'" \
    "ip -6 route | grep -q '^default via fe80::2 dev eth0'" "ping6 -c 2 -W 2 fec0::2" \
    "wget -q -O - http://[fec0::2]:@HP@/hello | grep -qx 'hello from the host'" \
    "ip addr show lo | grep -q 'inet6 ::1/128'" "ip route | grep -q '^default via 10.0.2.2 dev eth0'" \
    "nettest -s 8080" \
    "host:python3 scripts/net-host-server.py --echo-check @FP@" \
    "blktest /dev/vdb" "ext2test /dev/vda1 /mnt" "mkdir -p /mnt && mount -t ext2 /dev/vda1 /mnt && xattrtest /mnt && acltest /mnt && locktest /mnt/lockf && umount /mnt" "blktest -w /dev/vdb 77" \
    "grep -q '^violations 0' /proc/lockdep" || rc=1
fsck_img "$B/ext2.img" || rc=1
python3 scripts/qemu-test.py "$ARCH" --log "build/test-diskroot-$ARCH.log" --disk "$B/root.img" --disk "$B/scratch.img" \
    --cmdline "root=/dev/vda1" "$@" \
    "grep -q '^/dev/vda1 / ext2 rw' /proc/mounts" "blktest -v /dev/vdb 77" \
    libctest dyntest mapprivtest filetest vfstest vmtest sysfstest "permtest /tmp" logintest "mkdir /root/e2 && ext2test -d /root/e2 && rmdir /root/e2" \
    "xattrtest /root" "mkdir -p -m 755 /aclt && acltest /aclt" "locktest /root/lockf" "xattrtest -w /root" "acltest -w /aclt" "cp -a /usr /root/usr2 && echo persist > /root/keep && sync" \
    "grep -q '^violations 0' /proc/lockdep" || rc=1
python3 scripts/qemu-test.py "$ARCH" --log "build/test-diskroot2-$ARCH.log" --disk "$B/root.img" \
    --cmdline "root=/dev/vda1" "$@" \
    "grep -qx persist /root/keep" "xattrtest -r /root" "acltest -r /aclt && rmdir /aclt" "diff -r /usr /root/usr2" "rm -rf /root/usr2 /root/keep" || rc=1
if grep -aq "not cleanly unmounted" "build/test-diskroot2-$ARCH.log"; then echo "FAIL   clean shutdown of the disk root"; rc=1
else echo "PASS   clean shutdown of the disk root"; fi
fsck_img "$B/root.img" || rc=1
# M39: real audio samples and no-card boot.
scripts/snd-qemu-tests.sh "$ARCH" "$@" || rc=1
exit $rc
