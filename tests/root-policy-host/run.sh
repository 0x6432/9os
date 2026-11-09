#!/bin/sh
# Host-only parser and kernel syntax checks. No emulator or guest.
set -eu
cd "$(dirname "$0")/../.."
tmp=$(mktemp -d /tmp/9os-root-policy-XXXXXX)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
clang -std=c11 -Wall -Wextra -Werror -Ikernel/include tests/root-policy-host/test.c -o "$tmp/test"
"$tmp/test"
case "$(uname -m)" in
 x86_64|amd64) tests/root-policy-host/dispatch.sh;;
 *) echo "SKIP production kmain dispatch control: requires x86-64 host";;
esac
for arch in x86_64 aarch64 riscv64; do
 case "$arch" in
  x86_64) target=x86_64-unknown-none-elf;;
  aarch64) target=aarch64-unknown-none-elf;;
  riscv64) target=riscv64-unknown-none-elf;;
 esac
 clang --target="$target" -std=c2x -ffreestanding -fno-builtin -nostdlib -fsyntax-only \
  -Wall -Wextra -Wno-unused-parameter -Ikernel/include -Ithird_party/limine \
  -I"kernel/arch/$arch/include" -D"__9OS_ARCH_${arch}__" kernel/core/main.c
 echo "kernel syntax PASS $arch"
done
