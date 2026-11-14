#!/usr/bin/env python3
"""Generate and inspect an inert, strict-disk-root packaging prototype.

This creates *no ISO or disk*, performs no boot, and does not change Makefile or
any existing workflow.  A trusted private parent is required.  Same-user races,
host isolation, disk identity, firmware and runtime behavior are not covered.
"""
import argparse
import hashlib
import json
import os
import pathlib
import stat
import sys

CONFIG = b'''timeout: 0
verbose: no

/9os
    protocol: limine
    path: boot():/boot/kernel.elf
    module_path: boot():/boot/initramfs.cpio
    module_string: initramfs
    cmdline: root=/dev/vda1 root_required=1
'''
FILES = frozenset(('initramfs.cpio', 'limine.conf', 'manifest.json', 'READY'))


def record(name, mode, ino, nlink):
    name = name.encode('ascii') + b'\0'
    fields = (ino, mode, 0, 0, nlink, 0, 0, 0, 0, 0, 0, len(name), 0)
    header = b'070701' + b''.join(f'{n:08x}'.encode('ascii') for n in fields)
    result = header + name
    return result + b'\0' * (-len(result) % 4)


def archive_bytes():
    # Only an empty root and the newc end marker; no fallback userspace.
    data = record('.', stat.S_IFDIR | 0o755, 1, 2) + record('TRAILER!!!', 0, 0, 1)
    return data + b'\0' * (-len(data) % 512)


def manifest_bytes():
    obj = {'version': 1, 'purpose': 'unbooted-host-only-prototype',
           'cmdline': 'root=/dev/vda1 root_required=1',
           'files': {'initramfs.cpio': hashlib.sha256(archive_bytes()).hexdigest(),
                     'limine.conf': hashlib.sha256(CONFIG).hexdigest()}}
    return (json.dumps(obj, sort_keys=True, indent=2) + '\n').encode()


def write_new(path, data):
    # The containing directory is newly created and is expected to be private.
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'wb') as out:
        out.write(data)
        out.flush()
        os.fsync(out.fileno())


def create(destination):
    destination = pathlib.Path(destination)
    # Refuse all existing paths, including stale, partial or symlinked outputs.
    os.mkdir(destination, 0o700)
    # Validate the complete payload before publishing READY. A failed creation
    # retains the incomplete files as evidence and removes our marker if made.
    write_new(destination / 'initramfs.cpio', archive_bytes())
    write_new(destination / 'limine.conf', CONFIG)
    write_new(destination / 'manifest.json', manifest_bytes())
    verify_contents(destination, ready=False)
    try:
        write_new(destination / 'READY', b'host-only prototype; not boot approval\n')
        verify(destination)
    except BaseException:
        try:
            (destination / 'READY').unlink(missing_ok=True)
        except OSError:
            # Do not hide cleanup failure: a marker alone is never proof of success.
            raise
        raise


def verify_contents(destination, ready):
    destination = pathlib.Path(destination)
    directory = destination.lstat()
    if (not stat.S_ISDIR(directory.st_mode) or
            stat.S_IMODE(directory.st_mode) != 0o700 or directory.st_uid != os.geteuid()):
        raise ValueError('prototype path must be an owned, private directory')
    expected = {'initramfs.cpio': archive_bytes(), 'limine.conf': CONFIG,
                'manifest.json': manifest_bytes()}
    if ready:
        expected['READY'] = b'host-only prototype; not boot approval\n'
    if {p.name for p in destination.iterdir()} != set(expected):
        raise ValueError('prototype incomplete or has unexpected entries')
    for name, content in expected.items():
        p = destination / name
        st = p.lstat()
        if (not stat.S_ISREG(st.st_mode) or stat.S_IMODE(st.st_mode) != 0o600 or
                st.st_uid != os.geteuid() or st.st_nlink != 1 or p.read_bytes() != content):
            raise ValueError('prototype file differs or has unsafe metadata: ' + name)


def verify(destination):
    verify_contents(destination, ready=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['create', 'verify'])
    parser.add_argument('destination', type=pathlib.Path)
    args = parser.parse_args()
    (create if args.action == 'create' else verify)(args.destination)
    print('PASS host-only strict disk-root prototype; no ISO, disk or boot verified')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        print('disk-root prototype refused: ' + str(error), file=sys.stderr)
        sys.exit(1)
