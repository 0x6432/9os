#!/usr/bin/env bash
# Stage independent file copies before newc packing: the kernel's initramfs
# loader does not reconstruct hardlinks. This does not preserve hardlink
# semantics or capability xattrs and is not a fixture security audit.
set -euo pipefail
if [ "$#" -ne 2 ]; then echo 'usage: initramfs-pack.sh ROOT OUTPUT' >&2; exit 2; fi
root=$1
out=$2
[ -d "$root" ] || { echo 'initramfs root is not a directory' >&2; exit 1; }
stage=$(mktemp -d "${TMPDIR:-/tmp}/9os-initramfs.XXXXXXXX")
tmpout=''
cleanup() { rm -rf -- "$stage"; if [ -n "$tmpout" ]; then rm -f -- "$tmpout"; fi; }
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
mkdir "$stage/root"
cp -a --no-preserve=links --reflink=auto -- "$root/." "$stage/root/"
tmpout=$(mktemp "${out}.tmp.XXXXXXXX")
(cd "$stage/root" && find . -print0 | cpio -0 -o -H newc -R 0:0 --quiet) > "$tmpout"
mv -f -- "$tmpout" "$out"
tmpout=''
