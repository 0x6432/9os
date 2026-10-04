#!/bin/sh
# Set up the 9os build environment using the distro package manager (no QEMU compile),
# fetch Limine 11 (binary) + uACPI, then build the userland for the requested arches.
set -e
TOOLS=${TOOLS:-$HOME/.cache/9os-tools}
cd "$(dirname "$0")/.."
if command -v apt-get >/dev/null; then
    sudo apt-get install -y clang lld llvm xorriso cpio mtools make gcc git curl bzip2 xz-utils \
        qemu-system-x86 qemu-system-misc qemu-system-arm qemu-efi-aarch64 qemu-efi-riscv64 || true
elif command -v pacman >/dev/null; then
    sudo pacman -S --needed --noconfirm clang lld llvm libisoburn cpio mtools make gcc git curl \
        qemu-system-x86 qemu-system-riscv qemu-system-aarch64 edk2-ovmf edk2-aarch64 edk2-riscv64 || true
elif command -v dnf >/dev/null; then
    sudo dnf install -y clang lld llvm xorriso cpio mtools make gcc git curl bzip2 glibc-static \
        qemu-system-x86 qemu-system-riscv qemu-system-aarch64 edk2-aarch64 edk2-riscv64 || true
elif command -v brew >/dev/null; then
    brew install llvm lld xorriso cpio mtools qemu
fi
command -v qemu-system-x86_64 >/dev/null || echo "warning: QEMU not found in PATH - install it with your package manager"
mkdir -p "$TOOLS"
LIMINE_TAG=v11.4.1-binary
[ -d "$TOOLS/limine-bin" ] || { git clone -q --depth 1 --branch $LIMINE_TAG https://github.com/limine-bootloader/limine.git "$TOOLS/limine-bin"; make -C "$TOOLS/limine-bin" >/dev/null; }
[ -d "$TOOLS/uACPI" ] || { git clone -q https://github.com/uACPI/uACPI.git "$TOOLS/uACPI"; (cd "$TOOLS/uACPI" && git checkout -q fd92d3f); }
ln -sfn "$TOOLS/limine-bin" third_party/limine-bin
ln -sfn "$TOOLS/uACPI" third_party/uACPI
for a in ${ARCHES:-x86_64 riscv64 aarch64}; do ARCH=$a userland/build-all.sh; done
echo "environment ready"
