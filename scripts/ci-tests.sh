#!/bin/sh
# Run the in-tree regression tests in QEMU for ARCH (used by CI; works locally too).
# usage: scripts/ci-tests.sh ARCH [extra qemu-test.py options]
ARCH=${1:-x86_64}; shift
cd "$(dirname "$0")/.."
WL=; [ -e userland/build/ports-root-$ARCH/usr/bin/wltest ] && WL=wltest
exec python3 scripts/qemu-test.py "$ARCH" --log "build/test-$ARCH.log" "$@" \
    libctest cowtest ipctest ptytest inotifytest dyntest mapprivtest smptest faulttest \
    "bash -c 'a=(1 2 3); s=0; for i in \${a[@]}; do s=\$((s+i)); done; [ \$s = 6 ]'" \
    "echo hello | gzip | gunzip | grep -q hello" \
    ${WL} drmdemo
