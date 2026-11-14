#!/usr/bin/env python3
"""Install a locked, signed BUILD-HOST qsb; never build a Qt library.

The target Qt/Quick port still links its own libraries. Only the native shader
asset generator and its genuine upstream CMake metadata come from Alpine.
"""
import argparse
import concurrent.futures
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import tempfile


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--offline', action='store_true')
    args = ap.parse_args()
    top = Path(__file__).resolve().parents[2]
    spec = importlib.util.spec_from_file_location('alpine_build', top / 'userland/alpine/build.py')
    b = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(b)
    if platform.machine() != 'x86_64':
        raise SystemExit('qsb bootstrap requires an x86_64 build HOST; target architecture is independent')
    lock_path = Path(__file__).with_name('host-qsb-lock.json')
    raw = lock_path.read_bytes()
    lock = json.loads(raw)
    if (lock.get('profile'), lock.get('arch'), lock.get('alpine'), lock.get('minirootfs_sha256')) != ('host-qsb', 'x86_64', b.VERSION, b.MINI['x86_64']):
        raise ValueError('Host qsb lock/bootstrap mismatch')
    entries = b.validate_entries(lock['packages'], 'x86_64', True)
    metadata = b.validate_entries([lock['tool_metadata']], 'x86_64', True)[0]
    names = {e['url'].rsplit('/', 1)[1] for e in entries}
    if ('qt6-qtshadertools-6.8.3-r0.apk' not in names or
            not metadata['url'].endswith('/qt6-qtshadertools-dev-6.8.3-r0.apk')):
        raise ValueError('Expected reviewed qsb6.8.3 runtime and genuine tool metadata')
    tools = Path(os.environ.get('TOOLS_DIR', '/data/tools')).resolve()
    parent = tools / 'host'; parent.mkdir(parents=True, exist_ok=True)
    root = parent / 'alpine-qsb-runtime-6.8.3'
    if root.is_symlink():
        raise ValueError('Refusing a symlink host tool root')
    guard = open(parent / 'alpine-qsb-runtime-6.8.3.lock', 'w')
    fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
    identity = hashlib.sha256(raw).hexdigest() + '\n'
    marker = root / '.9os-qsb-lock'
    loader = root / 'lib/ld-musl-x86_64.so.1'
    qsb = root / 'usr/lib/qt6/bin/qsb'
    cmake = root / 'usr/lib/cmake/Qt6ShaderToolsTools'
    def version():
        return subprocess.check_output([str(qsb), '--version'], text=True, timeout=15).strip()
    if marker.is_file() and marker.read_text() == identity and qsb.is_file() and (cmake / 'Qt6ShaderToolsToolsConfig.cmake').is_file():
        if version() != 'qsb 6.8.3':
            raise ValueError('Unexpected cached qsb version')
        print(cmake)
        return
    if root.exists() and not marker.is_file():
        raise ValueError('Refusing to replace an unmarked host tool root')
    cache = tools / 'dl/alpine/x86_64'; cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='qsb-install-', dir=parent) as tmp:
        tmp = Path(tmp)
        host = tmp / 'bootstrap'; host.mkdir()
        stage = tmp / 'root'; stage.mkdir()
        archive = b.mini('x86_64', cache, args.offline)
        b.unpack(archive, host); b.unpack(archive, stage)
        apk = [str(host / 'lib/ld-musl-x86_64.so.1'), '--library-path', f'{host}/lib:{host}/usr/lib', str(host / 'sbin/apk')]
        trust = ['--keys-dir', str(stage / 'etc/apk/keys')]
        def fetch(e):
            path = cache / e['url'].rsplit('/', 1)[1]
            b.download(e['url'], path, e['sha256'], args.offline)
            return str(path)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            files = list(pool.map(fetch, entries))
        metadata_file = fetch(metadata)
        subprocess.run(apk + trust + ['verify'] + files + [metadata_file], check=True, stdout=__import__('sys').stderr)
        empty = tmp / 'empty-repositories'; empty.touch()
        subprocess.run(apk + ['--root', str(stage), '--arch', 'x86_64'] + trust + ['--repositories-file', str(empty), '--no-network', '--no-scripts', '--no-chown', 'add'] + files, check=True, stdout=__import__('sys').stderr)
        # Tool metadata only, not a host SDK: verify its APK signature above,
        # then extract the genuine tool CMake files and their declared include
        # directory. Do not recursively install the development package's large
        # unrelated SDK dependency set or alter any imported target definition.
        subprocess.run(['tar', 'xzf', metadata_file, '--no-same-owner', '-C', str(stage),
            'usr/lib/cmake/Qt6ShaderToolsTools', 'usr/include/qt6/QtShaderTools'], check=True,
            stdout=__import__('sys').stderr)
        packaged = stage / 'usr/lib/qt6/bin/qsb'
        if not packaged.is_file() or not (stage / 'usr/lib/cmake/Qt6ShaderToolsTools/Qt6ShaderToolsToolsConfig.cmake').is_file():
            raise ValueError('Incomplete signed host tools package')
        native = packaged.parent / 'alpine-native/qsb'
        native.parent.mkdir()
        packaged.rename(native)
        # Preserve unmodified CMake package paths. The executable launcher isolates
        # its musl/Qt dependencies from both host-source Qt and target Qt libraries.
        packaged.write_text('#!/bin/sh\nexec ' + shlex.quote(str(loader)) + ' --library-path ' + shlex.quote(f'{root}/lib:{root}/usr/lib') + ' ' + shlex.quote(str(qsb.parent / 'alpine-native/qsb')) + ' "$@"\n')
        packaged.chmod(0o755)
        (stage / '.9os-qsb-lock').write_text(identity)
        if root.exists(): shutil.rmtree(root)
        stage.rename(root)
    if version() != 'qsb 6.8.3':
        raise ValueError('Unexpected signed qsb version')
    print(cmake)


if __name__ == '__main__':
    main()
