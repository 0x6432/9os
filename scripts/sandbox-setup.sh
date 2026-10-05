#!/bin/sh
# Rebuild the agent-sandbox environment (Amazon Linux 2023) from scratch: host packages, QEMU from
# Alpine edge (apk.static, usermode root in $TOOLS_DIR/alpine), Limine + uACPI, the userland for ARCHES.
set -e
TOOLS_DIR=${TOOLS_DIR:-/data/tools}; export TOOLS_DIR
cd "$(dirname "$0")/.."
sudo dnf install -y -q clang lld llvm xorriso mtools cpio expat-devel libffi-devel bison flex pkgconf gcc make \
    glibc-static git bzip2 xz >/dev/null
pip3 install -q meson ninja 2>/dev/null || pip3 install -q --user meson ninja
mkdir -p "$TOOLS_DIR/bin" "$TOOLS_DIR/dl" && cd "$TOOLS_DIR"
cd - >/dev/null
scripts/install-qemu-alpine.sh
[ -d "$TOOLS_DIR/limine-bin" ] || { git clone -q --depth 1 --branch v11.4.1-binary https://github.com/limine-bootloader/limine.git "$TOOLS_DIR/limine-bin"; make -C "$TOOLS_DIR/limine-bin" >/dev/null; }
[ -d "$TOOLS_DIR/uACPI" ] || { git clone -q https://github.com/uACPI/uACPI.git "$TOOLS_DIR/uACPI"; (cd "$TOOLS_DIR/uACPI" && git checkout -q fd92d3f); }
ln -sfn "$TOOLS_DIR/limine-bin" third_party/limine-bin
ln -sfn "$TOOLS_DIR/uACPI" third_party/uACPI
for a in ${ARCHES:-x86_64 riscv64 aarch64}; do ARCH=$a userland/build-all.sh > /tmp/build-$a.log 2>&1 || { echo "userland $a FAILED"; tail -20 /tmp/build-$a.log; }; done
echo "sandbox ready (export PATH=$TOOLS_DIR/bin:\$PATH)"
