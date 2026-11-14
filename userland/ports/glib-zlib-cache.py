#!/usr/bin/env python3
"""Prefetch GLib's unchanged, hash-pinned zlib wrap before Meson runs.

A mirror returning a different archive must never weaken the upstream pin.
Keep both the downloads cache and Meson's packagecache verified and atomic.
"""
import configparser
import hashlib
import http.client
import os
from pathlib import Path
import sys
import tempfile
import urllib.request

NAME = 'zlib-1.2.11.tar.gz'
SHA256 = 'c3e5e9fdd5004dcb542feda5ee4f0ff0744628baf8ed2dd5d66f8ca1197cb1a1'
UPSTREAM = 'https://zlib.net/fossils/' + NAME
MIRROR = 'https://distfiles.alpinelinux.org/distfiles/v3.14/' + NAME
MAX_BYTES = 4 * 1024 * 1024


def verified(data, digest):
    return len(data) <= MAX_BYTES and hashlib.sha256(data).hexdigest() == digest


def atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + '.', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(data)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def archive(urls, digest, caches, opener=urllib.request.urlopen):
    """One verified byte sequence; untrusted cache/download bytes never install."""
    data = None
    for path in caches:
        if path.is_file() and path.stat().st_size <= MAX_BYTES:
            candidate = path.read_bytes()
            if verified(candidate, digest):
                data = candidate
                break
    if data is None:
        failures = []
        for url in urls:
            try:
                with opener(url, timeout=30) as response:
                    candidate = response.read(MAX_BYTES + 1)
                if not verified(candidate, digest):
                    raise ValueError('archive size or SHA256 mismatch')
                data = candidate
                break
            except (OSError, ValueError, http.client.HTTPException) as error:
                failures.append(f'{url}: {error}')
        if data is None:
            raise ValueError('no verified zlib source: ' + '; '.join(failures))
    for path in caches:
        if (not path.is_file() or path.stat().st_size > MAX_BYTES
                or path.read_bytes() != data):
            atomic_write(path, data)
    return data


def prepare(source, downloads, opener=urllib.request.urlopen):
    wrap = configparser.ConfigParser(interpolation=None)
    wrap.read(source / 'subprojects/zlib.wrap')
    expected = {'directory': 'zlib-1.2.11', 'source_url': UPSTREAM,
                'source_filename': NAME, 'source_hash': SHA256}
    if not wrap.has_section('wrap-file') or any(
            wrap['wrap-file'].get(k) != v for k, v in expected.items()):
        raise ValueError('GLib zlib wrap changed; review its source pin before updating')
    archive([MIRROR, UPSTREAM], SHA256,
            [downloads / NAME, source / 'subprojects/packagecache' / NAME], opener)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: glib-zlib-cache.py GLIB_SOURCE DOWNLOADS')
    try:
        prepare(Path(sys.argv[1]), Path(sys.argv[2]))
    except (OSError, ValueError, configparser.Error) as error:
        raise SystemExit(str(error))
    print('GLib zlib wrap source cached with its unchanged upstream SHA256')
