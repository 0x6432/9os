#!/bin/sh
# Boot 9os in QEMU, type commands into the serial console, print the (ANSI-stripped) log.
# usage: scripts/smoke-test.sh [ARCH] [boot-wait-seconds] [command...]
ARCH=${1:-x86_64}; WAIT=${2:-8}; shift 2 2>/dev/null
SMP=${SMP:-4}
[ $# -eq 0 ] && set -- "uname -a" "nproc" "cat /proc/cpuinfo | head -20" "/bin/libctest" "poweroff"
{
    sleep "$WAIT"
    for c in "$@"; do printf '%s\n' "$c"; sleep "${STEP:-2}"; done
    sleep 3
} | timeout "${TIMEOUT:-120}" make -s ARCH="$ARCH" SMP="$SMP" ${QEMU_GPU+QEMU_GPU="$QEMU_GPU"} run QEMUFLAGS="-display none ${QEMUEXTRA:-}" 2>&1 |
    sed -e 's/\x1b\[[0-9;?]*[a-zA-Z]//g' -e 's/\r//g'
