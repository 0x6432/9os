#!/usr/bin/env python3
"""Host-only check of an already-extracted strict-profile tree, NOT an ISO verifier.

It does not invoke an extractor, inspect ISO directory records, authenticate the
kernel or tools, or prove that this tree came from any particular ISO. The caller
must use a trusted private, stable tree; same-user/ancestor races are out of scope.
"""
import argparse
import hashlib
import os
from pathlib import Path
import stat


def check_directory(fd, private=False):
    st = os.fstat(fd)
    if st.st_uid != os.geteuid() or st.st_mode & (0o077 if private else 0o022):
        raise ValueError("extracted directory has unsafe owner or permissions")

# Independent constants: not imported from the prototype's generator or manifest.
EXPECTED_CONFIG = b'''timeout: 0
verbose: no

/9os
    protocol: limine
    path: boot():/boot/kernel.elf
    module_path: boot():/boot/initramfs.cpio
    module_string: initramfs
    cmdline: root=/dev/vda1 root_required=1
'''
ARCHIVE_SHA256 = 'c03280875bf409e63833f3d0b12ec8016fdcc3b21ae8727f602e2961cb316d39'
ARCHIVE_SIZE = 512


def read_single(root_fd, names, limit):
    """Walk relative components no-follow; refuse special files and hard links."""
    opened = []
    parent = root_fd
    try:
        for name in names[:-1]:
            fd = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent)
            opened.append(fd)
            check_directory(fd)
            parent = fd
        # Avoid opening an already-visible special file. This is not a race-free
        # defence against hostile writers; a stable private tree is required.
        visible = os.stat(names[-1], dir_fd=parent, follow_symlinks=False)
        if not stat.S_ISREG(visible.st_mode):
            raise ValueError("extracted path is not a regular file")
        fd = os.open(names[-1], os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW, dir_fd=parent)
        opened.append(fd)
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode) or before.st_uid != os.geteuid() or before.st_mode & 0o022 or before.st_nlink != 1 or before.st_size > limit:
            raise ValueError('unsafe or oversized extracted file: ' + '/'.join(names))
        data = b''
        while len(data) <= limit:
            part = os.read(fd, min(4096, limit + 1 - len(data)))
            if not part:
                break
            data += part
        after = os.fstat(fd)
        if len(data) > limit or (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise ValueError('extracted file changed during reading: ' + '/'.join(names))
        return data
    finally:
        for fd in reversed(opened):
            os.close(fd)


def check(root):
    root = os.fsdecode(root)
    if root.endswith(os.sep) and root != os.sep:
        raise ValueError('trailing slash is not accepted')
    root_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        check_directory(root_fd, private=True)
        config = read_single(root_fd, ('boot', 'limine', 'limine.conf'), len(EXPECTED_CONFIG))
        archive = read_single(root_fd, ('boot', 'initramfs.cpio'), ARCHIVE_SIZE)
    finally:
        os.close(root_fd)
    if config != EXPECTED_CONFIG:
        raise ValueError('extracted Limine configuration differs from strict profile')
    if len(archive) != ARCHIVE_SIZE or hashlib.sha256(archive).hexdigest() != ARCHIVE_SHA256:
        raise ValueError('extracted initramfs differs from strict profile')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('extracted_tree', type=Path)
    args = parser.parse_args()
    try:
        check(args.extracted_tree)
    except (OSError, ValueError) as error:
        parser.exit(1, 'extracted-tree check refused: ' + str(error) + '\n')
    print('PASS extracted config and archive only; ISO, kernel and guest not verified')


if __name__ == '__main__':
    main()
