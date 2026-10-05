#!/bin/sh
# Install QEMU (x86_64/riscv64/aarch64 + firmware) from Alpine edge into $TOOLS_DIR/alpine without
# root (apk.static --usermode) and put wrappers in $TOOLS_DIR/bin. Used by the sandbox and by CI,
# so both boot with the same QEMU/edk2 (Ubuntu 24.04's riscv64 edk2 + QEMU 8.2 make Limine panic
# with "BSP hart does not advertise MMU support").
set -e
TOOLS_DIR=${TOOLS_DIR:-/data/tools}
mkdir -p "$TOOLS_DIR/bin" && cd "$TOOLS_DIR"
if [ ! -x bin/qemu-system-x86_64 ]; then
    M=https://dl-cdn.alpinelinux.org/alpine/edge/main/x86_64
    V=$(curl -s $M/APKINDEX.tar.gz | tar xz -O APKINDEX 2>/dev/null | awk '/^P:apk-tools-static$/{f=1} f&&/^V:/{print; exit}' | sed 's/V://')
    curl -s -o apk.apk $M/apk-tools-static-$V.apk && mkdir -p apkx && tar xzf apk.apk -C apkx 2>/dev/null || true
    R=$TOOLS_DIR/alpine
    ./apkx/sbin/apk.static -X https://dl-cdn.alpinelinux.org/alpine/edge/main -X https://dl-cdn.alpinelinux.org/alpine/edge/community \
        -U --allow-untrusted --usermode -p $R --initdb add qemu-system-x86_64 qemu-system-riscv64 qemu-system-aarch64 \
        qemu-hw-display-virtio-gpu qemu-hw-display-virtio-gpu-pci >/tmp/apk.log 2>&1 || tail -5 /tmp/apk.log
    for a in x86_64 riscv64 aarch64; do
        printf '#!/bin/sh\nR=%s\nexport QEMU_MODULE_DIR=$R/usr/lib/qemu\nexec $R/lib/ld-musl-x86_64.so.1 --library-path $R/lib:$R/usr/lib $R/usr/bin/qemu-system-%s -L $R/usr/share/qemu "$@"\n' $R $a > bin/qemu-system-$a
        chmod +x bin/qemu-system-$a
    done
fi
mkdir -p "$TOOLS_DIR/share" && ln -sfn "$TOOLS_DIR/alpine/usr/share/qemu" "$TOOLS_DIR/share/qemu"
"$TOOLS_DIR/bin/qemu-system-riscv64" --version | head -1
