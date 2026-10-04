#!/bin/sh
# Fetch pinned third-party dependencies (not committed to the repo).
set -e
cd "$(dirname "$0")/.."
LIMINE_BRANCH=v9.x-binary
UACPI_REF=${UACPI_REF:-fd92d3f}   # uACPI 6.1.1
if [ ! -d third_party/limine-bin ]; then
    git clone --depth 1 --branch "$LIMINE_BRANCH" https://github.com/limine-bootloader/limine.git third_party/limine-bin
    make -C third_party/limine-bin >/dev/null
fi
if [ ! -d third_party/uACPI ]; then
    git clone https://github.com/uACPI/uACPI.git third_party/uACPI
    (cd third_party/uACPI && git checkout -q "$UACPI_REF")
fi
echo "deps ready"
