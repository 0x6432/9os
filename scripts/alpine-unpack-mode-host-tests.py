#!/usr/bin/env python3
"""Check miniroot modes and ownership without installing packages or running a VM."""
import importlib.util
import io
import os
import pathlib
import stat
import subprocess
import sys
import tarfile
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
module = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'userland/alpine/build.py'
spec = importlib.util.spec_from_file_location('alpine_builder', module)
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)

with tempfile.TemporaryDirectory(prefix='9os-unpack-test-') as tmp:
    temp = pathlib.Path(tmp)
    archive = temp / 'fixture.tar.gz'
    entries = [
        ('tmp', 'dir', 0o1777), ('var', 'dir', 0o755),
        ('var/tmp', 'dir', 0o1777), ('helper', 'file', 0o775),
        ('secret', 'file', 0o640), ('linked', 'symlink', 0o777),
    ]
    with tarfile.open(archive, 'w:gz') as tar:
        for name, kind, mode in entries:
            member = tarfile.TarInfo(name)
            member.uid, member.gid, member.mode = 12345, 23456, mode
            if kind == 'dir':
                member.type = tarfile.DIRTYPE
                tar.addfile(member)
            elif kind == 'symlink':
                member.type, member.linkname = tarfile.SYMTYPE, 'helper'
                tar.addfile(member)
            else:
                member.size = 4
                tar.addfile(member, io.BytesIO(b'test'))
    errors = []
    for mask in (0o022, 0o077):
        dest = temp / ('root-' + format(mask, '03o'))
        dest.mkdir()
        previous_umask = os.umask(mask)
        try:
            builder.unpack(archive, dest)
        finally:
            os.umask(previous_umask)
        for name, kind, mode in entries:
            path = dest / name
            metadata = path.lstat()
            if (metadata.st_uid, metadata.st_gid) != (os.geteuid(), os.getegid()):
                errors.append((oct(mask), name, 'owner', metadata.st_uid, metadata.st_gid))
            if kind != 'symlink' and stat.S_IMODE(metadata.st_mode) != mode:
                errors.append((oct(mask), name, 'mode', oct(stat.S_IMODE(metadata.st_mode)), oct(mode)))
            if kind == 'file' and path.read_bytes() != b'test':
                errors.append((oct(mask), name, 'content'))
            if kind == 'symlink' and (not stat.S_ISLNK(metadata.st_mode) or os.readlink(path) != 'helper'):
                errors.append((oct(mask), name, 'symlink'))
    bad = temp / 'bad.tar.gz'
    bad.write_bytes(b'not an archive')
    dest = temp / 'bad-root'
    dest.mkdir()
    try:
        builder.unpack(bad, dest)
    except subprocess.CalledProcessError:
        pass
    else:
        errors.append(('bad archive unexpectedly accepted',))
    if errors:
        for error in errors:
            print('FAIL', error)
        sys.exit(1)
    print('PASS: mode, content, ownership, symlink, and error handling under umasks 022/077')
