#!/usr/bin/env python3
"""Experimental newc/stage comparison for trusted, stable local build trees.

This is not a kernel extraction model, a sandbox, or a race-free audit.
"""
import os
import stat
import sys

CHUNK = 1024 * 1024


def identity(st):
    return (st.st_dev, st.st_ino, st.st_mode, st.st_size,
            st.st_mtime_ns, st.st_ctime_ns)


def read_exact(f, size):
    data = f.read(size)
    if len(data) != size:
        raise ValueError('truncated archive')
    return data


def align4(f):
    padding = read_exact(f, (-f.tell()) % 4)
    if any(padding):
        raise ValueError('nonzero newc padding')


def expected_names(root):
    names = set()
    pending = [(b'.', root)]
    while pending:
        rel, path = pending.pop()
        names.add(rel)
        st = os.lstat(path)
        if stat.S_ISDIR(st.st_mode):
            with os.scandir(path) as scan:
                children = [entry.name for entry in scan]
            for name in children:
                childrel = name if rel == b'.' else rel + b'/' + name
                pending.append((childrel, os.path.join(path, name)))
    return names


def stage_path(root, name):
    if name == b'.':
        return root, None
    # The current loader formats '/' + name + NUL into char path[512].
    # Reject paths it would silently truncate; this is not a VFS model.
    if len(name) > 510:
        raise ValueError('archive path exceeds kernel path buffer')
    if name.startswith(b'/'):
        raise ValueError('noncanonical archive path')
    parts = name.split(b'/')
    if any(p in (b'', b'.', b'..') for p in parts):
        raise ValueError('noncanonical archive path')
    parent = b'.' if len(parts) == 1 else b'/'.join(parts[:-1])
    return os.path.join(root, *parts), parent


def check(archive_path, root):
    if not hasattr(os, 'O_NOFOLLOW') or not hasattr(os, 'O_NONBLOCK'):
        raise ValueError('required no-follow/nonblocking open unavailable')
    flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | getattr(os, 'O_CLOEXEC', 0)
    root = os.fsencode(root)
    expected = expected_names(root)
    seen = set()
    seen_dirs = set()
    with os.fdopen(os.open(archive_path, flags), 'rb') as archive:
        initial_archive = os.fstat(archive.fileno())
        if not stat.S_ISREG(initial_archive.st_mode):
            raise ValueError('archive is not a regular file')
        archive_size = initial_archive.st_size
        while True:
            header = read_exact(archive, 110)
            if header[:6] != b'070701':
                raise ValueError('invalid newc header')
            raw_fields = [header[6+i*8:14+i*8] for i in range(13)]
            if any(any(c not in b'0123456789abcdefABCDEF' for c in x) for x in raw_fields):
                raise ValueError('invalid newc numeric field')
            fields = [int(x, 16) for x in raw_fields]
            _, mode, uid, gid, nlink, _, size, _, _, rmajor, rminor, nsize, check_value = fields
            if nsize < 1 or nsize > 4096 or size > archive_size or check_value != 0:
                raise ValueError('unsupported newc size or checksum field')
            raw_name = read_exact(archive, nsize)
            if not raw_name.endswith(b'\0') or b'\0' in raw_name[:-1]:
                raise ValueError('invalid archive name')
            name = raw_name[:-1]
            align4(archive)
            if name == b'TRAILER!!!':
                if size != 0 or seen != expected:
                    raise ValueError('trailer or entry set mismatch')
                break
            path, parent = stage_path(root, name)
            if name not in expected or name in seen or (parent is not None and parent not in seen_dirs):
                raise ValueError('unexpected, duplicate or out-of-order entry')
            seen.add(name)
            st = os.lstat(path)
            # devices_init runs before unpacking. Conservatively allow only
            # its original directory modes, and no archive /dev descendants;
            # this is not an exhaustive model of boot-time VFS collisions.
            if name.startswith(b'dev/'):
                raise ValueError('preexisting kernel path: archive /dev descendant')
            expected_directory_mode = {b'dev': 0o755, b'tmp': 0o1777}.get(name)
            if expected_directory_mode is not None and (
                    not stat.S_ISDIR(st.st_mode) or
                    stat.S_IMODE(st.st_mode) != expected_directory_mode):
                raise ValueError('preexisting kernel path: unexpected directory type or mode')
            if mode != st.st_mode or uid != 0 or gid != 0:
                raise ValueError('archive mode or normalized owner mismatch')
            if stat.S_ISDIR(st.st_mode):
                seen_dirs.add(name)
                if size: raise ValueError('directory payload mismatch')
            elif stat.S_ISREG(st.st_mode):
                if nlink != 1 or size != st.st_size:
                    raise ValueError('regular file size or link count mismatch')
                with os.fdopen(os.open(path, flags), 'rb') as source:
                    before = os.fstat(source.fileno())
                    if identity(before) != identity(st):
                        raise ValueError('stage file changed before archive comparison')
                    remaining = size
                    while remaining:
                        count = min(CHUNK, remaining)
                        if read_exact(archive, count) != read_exact(source, count):
                            raise ValueError('archive file payload mismatch')
                        remaining -= count
                    if source.read(1) or identity(os.fstat(source.fileno())) != identity(before):
                        raise ValueError('stage file changed during archive comparison')
            elif stat.S_ISLNK(st.st_mode):
                target = os.readlink(path)
                if size != len(target) or read_exact(archive, size) != target:
                    raise ValueError('archive symlink target mismatch')
            elif stat.S_ISCHR(st.st_mode) or stat.S_ISBLK(st.st_mode):
                # Kernel MKDEV/MAJOR/MINOR encode 12 major and 8 minor bits.
                if rmajor > 0xfff or rminor > 0xff:
                    raise ValueError('kernel device number cannot represent archive device')
                if size or (rmajor, rminor) != (os.major(st.st_rdev), os.minor(st.st_rdev)):
                    raise ValueError('archive device metadata mismatch')
            elif stat.S_ISFIFO(st.st_mode):
                if size: raise ValueError('fifo payload mismatch')
            else:
                raise ValueError('unsupported staged file type')
            align4(archive)
        while True:
            trailing = archive.read(CHUNK)
            if not trailing: break
            if any(trailing): raise ValueError('nonzero data after trailer')
        if identity(os.fstat(archive.fileno())) != identity(initial_archive):
            raise ValueError('archive changed during verification')


if __name__ == '__main__':
    try:
        if len(sys.argv) != 3:
            raise ValueError('usage: initramfs-archive-check.py ARCHIVE STAGE')
        check(sys.argv[1], sys.argv[2])
    except (OSError, ValueError) as error:
        print(f'initramfs archive verification failed: {error}', file=sys.stderr)
        sys.exit(1)
