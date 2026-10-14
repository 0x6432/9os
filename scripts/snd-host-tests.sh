#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/host-tests
${HOST_CC:-cc} -std=c11 -Wall -Wextra -Werror -Ikernel/include tests/snd-host/abi.c -o build/host-tests/snd-abi
build/host-tests/snd-abi
echo 'snd-host: Linux PCM sizes, offsets and ioctl encodings passed'
