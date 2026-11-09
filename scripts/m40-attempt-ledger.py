#!/usr/bin/env python3
"""Conditional per-directory reservation primitive; not a launch gate.

The caller must independently verify and bind the approved contract, keep the
one pre-provisioned directory and ancestors stable and exclusively controlled,
and ensure persistent storage cannot roll back. No launch may occur until
reserve() returns, but this function cannot enforce that rule, prevent use of a
different directory, or prevent multiple launches after one success. A prior or
incomplete marker is never removed here. An ambiguous failure requires external
reconciliation, never an automatic retry.
"""
import argparse
import json
import os
import re
import stat
import sys

NAME = 'attempt.json'
SHA256 = re.compile(r'[0-9a-f]{64}\Z')


class ReservationError(Exception):
    pass


def open_private_dirs(path):
    """Open every ancestor without symlink traversal and retain descriptors.

    Ancestor replacement and same-UID interference still require external
    protection; directory descriptors are not a trust decision.
    """
    if not isinstance(path, str) or not path.startswith('/') or path == '/' or '\x00' in path:
        raise ReservationError('ledger directory must be an absolute non-root path without NUL')
    parts = path[1:].split('/')
    if any(part in ('', '.', '..') for part in parts):
        raise ReservationError('ledger directory must not contain empty, . or .. components')
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
    fds = [os.open('/', flags)]
    try:
        for part in parts:
            fds.append(os.open(part, flags, dir_fd=fds[-1]))
        info = os.fstat(fds[-1])
        if info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700:
            raise ReservationError('ledger directory must be owned by caller and mode 0700')
        return fds
    except BaseException:
        for fd in reversed(fds): os.close(fd)
        raise


def reserve(directory, contract_sha256):
    """Attempt one per-directory reservation; do not launch on failure.

    Success means the record and opened directory ancestry were fsynced
    bottom-up. No assumption about persistent media, rollback or exclusive
    control is validated here. A crash or exception can leave an incomplete
    marker or ambiguous durability: quarantine and reconcile externally,
    never delete a marker or automatically retry. This primitive is not
    integrated into the launcher and cannot authorize or limit launches.
    """
    if not isinstance(contract_sha256, str) or not SHA256.fullmatch(contract_sha256):
        raise ReservationError('contract digest must be 64 lowercase hexadecimal characters')
    data = (json.dumps({'version': 1, 'contract_sha256': contract_sha256},
                       sort_keys=True, separators=(',', ':')) + '\n').encode('ascii')
    dirs = open_private_dirs(directory)
    try:
        dfd = dirs[-1]
        try:
            fd = os.open(NAME, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
                         0o600, dir_fd=dfd)
        except FileExistsError as e:
            raise ReservationError('attempt already reserved or marker exists') from e
        try:
            view = memoryview(data)
            while view:
                n = os.write(fd, view)
                if n <= 0:
                    raise OSError('zero-length ledger write')
                view = view[n:]
            os.fsync(fd)
        finally:
            os.close(fd)
        for ancestor in reversed(dirs): os.fsync(ancestor)
    finally:
        for ancestor in reversed(dirs): os.close(ancestor)


def main():
    parser = argparse.ArgumentParser(description='Reserve one attempt; never executes a guest.')
    parser.add_argument('directory', help='trusted absolute directory, owned by caller, mode 0700')
    parser.add_argument('--contract-sha256', required=True, help='independently verified protocol digest')
    args = parser.parse_args()
    try:
        reserve(args.directory, args.contract_sha256)
    except (OSError, ReservationError) as e:
        print('FAIL attempt reservation: ' + str(e), file=sys.stderr)
        return 1
    print('DIRECTORY_RESERVATION_RECORDED; this alone does not authorize or limit a boot')
    return 0


if __name__ == '__main__':
    sys.exit(main())
