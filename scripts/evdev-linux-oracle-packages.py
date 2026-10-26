#!/usr/bin/env python3
"""Acquire one explicit signed Linux-reference closure; never build libraries.

Only this helper's separate empty target is installed. The oracle's S-root
consumer, BusyBox and musl are not arguments and are never modified here.
"""
import argparse
import gzip
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import struct
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('linux_oracle_builder', ROOT / 'userland/alpine/build.py')
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)
LOCK = ROOT / 'userland/ports/linux-oracle-lock.json'
VERSION = '6.12.114-r0'
RELEASE = '6.12.114-0-virt'
NAMES = set('apk-tools busybox busybox-binsh ca-certificates-bundle cryptsetup-libs device-mapper-libs json-c kmod kmod-libs lddtree libapk2 libblkid libcrypto3 libeconf libssl3 libuuid linux-virt mdev-conf mkinitfs musl scanelf ssl_client xz-libs zlib zstd-libs'.split())
NOARCH = {'busybox-binsh', 'ca-certificates-bundle', 'lddtree', 'mdev-conf'}
SHA = re.compile(r'[0-9a-f]{64}')
BASE = 'https://dl-cdn.alpinelinux.org/alpine/v3.22/main/x86_64/'


def require(ok, message):
    if not ok:
        raise ValueError(message)


def digest(path):
    return BUILD.digest(path)


def check_file(path, entry):
    require(path.is_file(), 'Required input missing: ' + str(path))
    require(path.stat().st_size == entry['bytes'] and digest(path) == entry['sha256'],
            'Input checksum/size mismatch: ' + str(path))


def load_lock(path):
    lock = json.loads(path.read_text())
    require((lock['format'], lock['purpose'], lock['arch'], lock['alpine']) ==
            (1, 'signed-linux-reference-evdev-oracle', 'x86_64', BUILD.VERSION), 'Lock bootstrap/purpose mismatch')
    mini = lock['native_mini']
    require(mini['sha256'] == BUILD.MINI['x86_64'] and mini['url'] ==
            f'{BUILD.BASE}/{BUILD.BRANCH}/releases/x86_64/alpine-minirootfs-{BUILD.VERSION}-x86_64.tar.gz',
            'Verifier mini must be independently SHA-pinned')
    require(mini['apk_version'] == '2.14.12', 'Unexpected native APK version')
    require({e['path'] for e in mini['binaries']} == {'sbin/apk', 'lib/ld-musl-x86_64.so.1'}
            and len(mini['binaries']) == 2 and len(mini['keys']) == 5, 'Invalid verifier inventory')
    require(len({e['file'] for e in mini['keys']}) == 5 and
            all(re.fullmatch(r'alpine-devel@lists\.alpinelinux\.org-[0-9a-f]+\.rsa\.pub', e['file'])
                for e in mini['keys']), 'Invalid trust-key inventory')
    kernel = lock['kernel']
    require((kernel['name'], kernel['version'], kernel['module_release']) ==
            ('linux-virt', VERSION, RELEASE), 'Only explicitly reviewed NEW114 reference is allowed')
    expected_paths = {'boot/vmlinuz-virt'} | {
        f'lib/modules/{RELEASE}/kernel/drivers/input/{n}.ko.gz' for n in ['evdev', 'misc/uinput']}
    require(len(kernel['contents']) == 3 and {e['path'] for e in kernel['contents']} == expected_paths,
            'Kernel/module inventory mismatch')
    entries = lock['packages']
    require(len(entries) == 25 and {e['name'] for e in entries} == NAMES, 'Missing/extra/duplicate closure identity')
    require(len({e['url'] for e in entries}) == 25, 'Duplicate APK filename')
    for e in entries:
        require(re.fullmatch(r'[A-Za-z0-9+_.-]+', e['name']) and
                re.fullmatch(r'[A-Za-z0-9+_.-]+', e['version']), 'Invalid package identity')
        require(e['url'] == BASE + e['name'] + '-' + e['version'] + '.apk', 'Unexpected APK URL/substitution')
        require(e['arch'] == ('noarch' if e['name'] in NOARCH else 'x86_64'), 'Unexpected package architecture')
        require(SHA.fullmatch(e['sha256']) and SHA.fullmatch(e['pkginfo_sha256']) and
                type(e['bytes']) is int and 0 < e['bytes'] <= 64 * 1024 * 1024, 'Invalid APK digest/size')
        require(isinstance(e['dependencies'], list) and isinstance(e['provides'], list) and
                all(isinstance(v, str) and v and '\n' not in v for v in e['dependencies'] + e['provides']),
                'Invalid dependency/provider metadata')
        if e['name'] == 'linux-virt':
            require(e['version'] == VERSION, 'Kernel APK version mismatch')
    return lock


def checked(command):
    result = subprocess.run(command, capture_output=True, text=True)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    result.check_returncode()
    return result


def bootstrap(lock, cache, work, offline, runner):
    mini = lock['native_mini']
    archive = cache / mini['url'].rsplit('/', 1)[-1]
    BUILD.download(mini['url'], archive, mini['sha256'], offline)
    require(digest(archive) == mini['sha256'], 'Verifier mini checksum mismatch')
    native = work / 'native'
    native.mkdir()
    BUILD.unpack(archive, native)
    for entry in mini['binaries']:
        check_file(native / entry['path'], entry)
    keys = native / 'etc/apk/keys'
    require({p.name for p in keys.iterdir()} == {e['file'] for e in mini['keys']}, 'Verifier trust-key set mismatch')
    for entry in mini['keys']:
        check_file(keys / entry['file'], entry)
    command = [str(native / 'lib/ld-musl-x86_64.so.1'), '--library-path',
               f'{native}/lib:{native}/usr/lib', str(native / 'sbin/apk'),
               '--keys-dir', str(keys), '--repositories-file', '/dev/null', '--no-network']
    version = runner(command + ['--version'])
    require(version.stdout.startswith('apk-tools 2.14.12, compiled for x86_64.'), 'Native verifier version mismatch')
    return command


def inspect_metadata(path, entry):
    # Called only AFTER every APK signature was verified. Never extract archives.
    with tarfile.open(path, 'r:gz', ignore_zeros=True) as archive:
        infos = [m for m in archive if m.name == '.PKGINFO']
        require(len(infos) == 1 and infos[0].isfile() and infos[0].size <= 1024 * 1024, 'Invalid PKGINFO member')
        raw = archive.extractfile(infos[0]).read()
    require(hashlib.sha256(raw).hexdigest() == entry['pkginfo_sha256'], 'PKGINFO checksum mismatch')
    info = {}
    for line in raw.decode('utf-8').splitlines():
        if ' = ' in line:
            key, value = line.split(' = ', 1)
            info.setdefault(key, []).append(value)
    require(info.get('pkgname') == [entry['name']] and info.get('pkgver') == [entry['version']]
            and info.get('arch') == [entry['arch']], 'Signed APK identity/version/architecture mismatch')
    require(sorted(info.get('depend', [])) == sorted(entry['dependencies']) and
            sorted(info.get('provides', [])) == sorted(entry['provides']), 'Signed dependency/provider mismatch')


def verify_contents(target, lock):
    results = []
    for entry in lock['kernel']['contents']:
        path = target / entry['path']
        require(path.resolve().is_relative_to(target.resolve()) and not path.is_symlink(), 'Unsafe installed kernel path')
        check_file(path, entry)
        if entry['path'].endswith('.ko.gz'):
            raw = gzip.decompress(path.read_bytes())
            require(len(raw) == entry['uncompressed_bytes'] and
                    hashlib.sha256(raw).hexdigest() == entry['uncompressed_sha256'], 'Module content checksum mismatch')
            require(raw[:6] == b'\x7fELF\x02\x01' and len(raw) >= 20 and
                    struct.unpack('<H', raw[18:20])[0] == entry['elf_machine'] == 62, 'Unexpected module ELF architecture')
            vermagic = re.search(rb'vermagic=([^\x00]+)', raw)
            require(vermagic is not None and vermagic[1].decode() == entry['vermagic'] and
                    entry['vermagic'].startswith(RELEASE + ' '), 'Wrong installed module ABI')
        results.append({'path': entry['path'], 'sha256': digest(path), 'bytes': path.stat().st_size})
    return results


def installed_inventory(target, lock):
    text = (target / 'lib/apk/db/installed').read_text()
    inventory = []
    for block in text.split('\n\n'):
        fields = dict(line.split(':', 1) for line in block.splitlines() if ':' in line)
        if fields:
            inventory.append((fields['P'], fields['V']))
    expected = [(e['name'], e['version']) for e in lock['packages']]
    require(sorted(inventory) == sorted(expected), 'Installed package set/version mismatch')


def empty_target(target):
    require(target.is_dir() and not target.is_symlink() and not any(target.iterdir()), 'Target must be separately EMPTY')


def assemble(lock_path, cache, target, offline=False, runner=checked):
    require(platform.machine() == 'x86_64', 'Linux reference verifier requires native x86_64')
    lock = load_lock(lock_path)
    empty_target(target)
    cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='linux-reference-verifier-') as tmp:
        work = Path(tmp)
        apk = bootstrap(lock, cache, work, offline, runner)
        files = []
        for entry in lock['packages']:
            path = cache / entry['url'].rsplit('/', 1)[-1]
            BUILD.download(entry['url'], path, entry['sha256'], offline)
            check_file(path, entry)
            files.append(str(path))
        # ALL content hashes and ALL native signatures precede metadata/solver/add.
        runner(apk + ['verify'] + files)
        for entry, path in zip(lock['packages'], files):
            inspect_metadata(Path(path), entry)
        dry = work / 'dry-run'
        (dry / 'etc/apk').mkdir(parents=True)
        (dry / 'lib/apk/db').mkdir(parents=True)
        for name in ['etc/apk/world', 'lib/apk/db/installed', 'lib/apk/db/triggers']:
            (dry / name).write_bytes(b'')
        # Strict native solver (not fetch's IGNORE_CONFLICT mode), no package install.
        flags = ['--root', str(dry), '--arch', 'x86_64', 'add', '--simulate', '--initdb',
                 '--no-scripts', '--no-chown', 'linux-virt=' + VERSION]
        runner(apk + flags + files)
        bookkeeping = {str(p.relative_to(dry)): p.stat().st_size for p in dry.rglob('*') if p.is_file() or p.is_symlink()}
        require(bookkeeping == {name: 0 for name in ['etc/apk/world', 'lib/apk/db/installed', 'lib/apk/db/triggers']},
                'Dry-run wrote package state')
        empty_target(target)
        # A failure here remains fatal; never rescue it or proceed to QEMU.
        runner(apk + ['--root', str(target), '--arch', 'x86_64', 'add', '--initdb',
                      '--no-scripts', '--no-chown', 'linux-virt=' + VERSION] + files)
        installed_inventory(target, lock)
        contents = verify_contents(target, lock)
    return {'purpose': lock['purpose'], 'reference_kernel': VERSION, 'module_release': RELEASE,
            'native_mini_sha256': lock['native_mini']['sha256'], 'packages': lock['packages'],
            'installed_contents': contents, 'historical_113_byte_equivalence': 'unknown'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', type=Path, required=True)
    # Keep the runner-owned cache outside dl, whose parent may be created by sudo runtime assembly.
    parser.add_argument('--cache', type=Path, default=Path(os.environ.get('TOOLS_DIR', ROOT / '.tools')) / 'linux-oracle')
    parser.add_argument('--lock', type=Path, default=LOCK)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--offline', action='store_true')
    args = parser.parse_args()
    result = assemble(args.lock, args.cache, args.target, args.offline)
    args.manifest.write_text(json.dumps(result, indent=2) + '\n')
    print('Explicit NEW Linux reference:', VERSION, '; 25 verified APKs; offline installation; modules', RELEASE)


if __name__ == '__main__':
    main()
