#!/bin/sh
# Recreate the 9os build environment from scratch (Amazon Linux 2023 / Fedora-like hosts).
# Installs host tools, builds QEMU 9.2.3 (x86_64, riscv64, aarch64) into /data/tools/qemu,
# fetches Limine + uACPI, then builds the userland for the requested arches.
set -e
TOOLS=${TOOLS:-/data/tools}
cd "$(dirname "$0")/.."
sudo dnf install -y clang lld llvm xorriso cpio mtools ninja-build glib2-devel pixman-devel \
    flex bison bzip2 patch diffutils perl glibc-static gcc make git
mkdir -p "$TOOLS"
if [ ! -x "$TOOLS/qemu/bin/qemu-system-riscv64" ]; then
    (cd "$TOOLS" && curl -sL https://download.qemu.org/qemu-9.2.3.tar.xz | tar xJ && cd qemu-9.2.3 &&
     ./configure --prefix="$TOOLS/qemu" --target-list=x86_64-softmmu,riscv64-softmmu,aarch64-softmmu \
         --disable-docs --disable-werror >/dev/null && make -j"$(nproc)" >/dev/null && make install >/dev/null)
fi
[ -d "$TOOLS/limine-bin" ] || { git clone -q --depth 1 --branch v9.x-binary https://github.com/limine-bootloader/limine.git "$TOOLS/limine-bin"; make -C "$TOOLS/limine-bin" >/dev/null; }
[ -d "$TOOLS/uACPI" ] || { git clone -q https://github.com/uACPI/uACPI.git "$TOOLS/uACPI"; (cd "$TOOLS/uACPI" && git checkout -q fd92d3f); }
ln -sfn "$TOOLS/limine-bin" third_party/limine-bin
ln -sfn "$TOOLS/uACPI" third_party/uACPI
for a in ${ARCHES:-x86_64 riscv64 aarch64}; do ARCH=$a userland/build-all.sh; done
echo "environment ready: export PATH=$TOOLS/qemu/bin:\$PATH"
