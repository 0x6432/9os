#!/bin/sh
# Build a GPT disk image with one ext2 partition (M30).
# usage: scripts/mkdisk.sh OUT SIZE_MB [ROOTDIR] [BLOCKSIZE]
#   ROOTDIR: directory copied into the filesystem (mke2fs -d), e.g. userland/root-x86_64
# needs sfdisk (util-linux) and mke2fs (e2fsprogs)
set -e
out=$1; mb=${2:-64}; dir=$3; bs=${4:-4096}
[ -n "$out" ] || { echo "usage: $0 OUT SIZE_MB [ROOTDIR] [BLOCKSIZE]" >&2; exit 1; }
command -v mke2fs >/dev/null || PATH=$PATH:/usr/sbin:/sbin
rm -f "$out" "$out.part"
truncate -s "${mb}M" "$out"
printf 'label: gpt\nstart=2048, size=%d, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=root\n' $(( (mb - 2) * 2048 )) |
    sfdisk -q "$out" >/dev/null
truncate -s "$(( mb - 2 ))M" "$out.part"
# plain ext2 (no htree, no resize inode): the feature set 9os implements
mke2fs -q -F -t ext2 -b "$bs" -O ^dir_index,^resize_inode -E root_owner=0:0 ${dir:+-d "$dir"} "$out.part"
if [ -n "$dir" ] && [ "$(id -u)" != 0 ]; then
    # mke2fs -d keeps host ownership (the builder's uid); the image is a root filesystem: root:root
    command -v debugfs >/dev/null || PATH=$PATH:/usr/sbin:/sbin
    (cd "$dir" && find . -mindepth 1 | sed 's|^\.||') | while IFS= read -r f; do
        printf 'sif "%s" uid 0\nsif "%s" gid 0\n' "$f" "$f"
    done > "$out.dbg"
    debugfs -w -f "$out.dbg" "$out.part" >/dev/null 2>&1
    rm -f "$out.dbg"
fi
dd if="$out.part" of="$out" bs=1M seek=1 conv=notrunc,sparse status=none
rm -f "$out.part"
