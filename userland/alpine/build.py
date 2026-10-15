#!/usr/bin/env python3
"""Build an isolated, signed Alpine runtime; never execute target package scripts."""
import argparse, concurrent.futures, fcntl, hashlib, json, os, pathlib, platform
import shutil, subprocess, tempfile, time, urllib.request

VERSION = '3.22.6'
BRANCH = 'v3.22'
BASE = 'https://dl-cdn.alpinelinux.org/alpine'
MINI = {
    'x86_64': '27694aaa55fd7a9e3ef596e0ad4eb66802308bb20172b17030cd5f4d8ae9bac2',
    'aarch64': '821565fa8f3953eefd12497b166b4b50add2f7c57fb312e75862f5867e06fefe',
    'riscv64': '72f7e69f6950c2799137a934fae61513016efa546afcb4a05540a505c4184150',
}
PACKAGES = ['qt6-qtdeclarative', 'dbus', 'font-dejavu']
TOP = pathlib.Path(__file__).resolve().parents[2]

def digest(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def download(url, path, sha=None):
    if path.exists() and (sha is None or digest(path) == sha):
        return
    part = path.with_suffix(path.suffix + '.part')
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=60) as r, open(part, 'wb') as f:
                shutil.copyfileobj(r, f)
            if sha and digest(part) != sha:
                raise ValueError('checksum mismatch: ' + url)
            part.replace(path)
            return
        except Exception:
            part.unlink(missing_ok=True)
            if attempt == 2:
                raise
            time.sleep(attempt + 1)

def mini(arch, cache):
    name = f'alpine-minirootfs-{VERSION}-{arch}.tar.gz'
    path = cache / name
    download(f'{BASE}/{BRANCH}/releases/{arch}/{name}', path, MINI[arch])
    return path

def unpack(archive, root):
    subprocess.run(['tar', 'xzf', str(archive), '--no-same-owner', '-C', str(root)], check=True)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--arch', choices=MINI, default=os.environ.get('ARCH', 'x86_64'))
    ap.add_argument('--lock', type=pathlib.Path, help='Replay exact signed APK URLs/hashes; no index refresh')
    args = ap.parse_args()
    if platform.machine() != 'x86_64':
        ap.error('bootstrap currently requires an x86_64 build host (all targets cross-installed)')
    start = time.monotonic()
    arch = args.arch
    cache = pathlib.Path(os.environ.get('TOOLS_DIR', '/data/tools')).resolve() / 'dl' / 'alpine' / arch
    cache.mkdir(parents=True, exist_ok=True)
    work = TOP / 'userland/build'
    work.mkdir(exist_ok=True)
    guard = open(work / f'alpine-{arch}.build.lock', 'w')
    fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
    root = TOP / f'userland/alpine-root-{arch}'
    if root.exists() and not (root / 'etc/9os-alpine.json').is_file():
        raise SystemExit('Refusing to replace an unmarked image: ' + str(root))
    with tempfile.TemporaryDirectory(prefix=f'alpine-{arch}-', dir=work) as tmp:
        tmp = pathlib.Path(tmp)
        host = tmp / 'host'; host.mkdir()
        unpack(mini('x86_64', cache), host)
        stage = tmp / 'root'; stage.mkdir()
        unpack(mini(arch, cache), stage)
        repos = tmp / 'repositories'
        repos.write_text('\n'.join(f'{BASE}/{BRANCH}/{r}' for r in ['main', 'community']) + '\n')
        apk = [str(host / 'lib/ld-musl-x86_64.so.1'), '--library-path',
               f'{host}/lib:{host}/usr/lib', str(host / 'sbin/apk')]
        trust = ['--keys-dir', str(host / 'etc/apk/keys')]
        if args.lock:
            lock = json.loads(args.lock.read_text())
            if (lock['arch'], lock['alpine'], lock['minirootfs_sha256']) != (arch, VERSION, MINI[arch]):
                raise ValueError('lock architecture/release/bootstrap mismatch')
            entries = lock['packages']
        else:
            cmd = apk + ['--root', str(host), '--arch', arch] + trust + [
                '--repositories-file', str(repos), '--no-cache', 'fetch', '--simulate', '--recursive', '--url'] + PACKAGES
            # --simulate is essential: apk2 --url alone downloads too, and omits
            # cached files from its URL output. Resolve from an empty directory.
            out = subprocess.check_output(cmd, text=True, cwd=tmp)
            entries = [{'url': u, 'sha256': None} for u in out.splitlines() if u.startswith('https://') and u.endswith('.apk')]
            if not entries or not any(u['url'].endswith('/qt6-qtdeclarative-6.8.3-r0.apk') for u in entries):
                raise ValueError('Expected the reviewed Alpine Qt6.8.3 package set')
        def fetch(entry):
            url = entry['url']
            allowed = tuple(f'{BASE}/{BRANCH}/{r}/{arch}/' for r in ['main', 'community'])
            if not url.startswith(allowed) or '/' in url.rsplit('/', 1)[-1] or not url.endswith('.apk'):
                raise ValueError('Unexpected package URL: ' + url)
            path = cache / url.rsplit('/', 1)[-1]
            download(url, path, entry['sha256'])
            return {'url': url, 'sha256': digest(path)}
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            entries = sorted(pool.map(fetch, entries), key=lambda e: e['url'])
        files = [str(cache / e['url'].rsplit('/', 1)[-1]) for e in entries]
        # Neither indexes nor packages may bypass Alpine signature verification.
        subprocess.run(apk + trust + ['verify'] + files, check=True)
        empty = tmp / 'empty-repositories'; empty.touch()
        subprocess.run(apk + ['--root', str(stage), '--arch', arch] + trust + [
            '--repositories-file', str(empty), '--no-network', '--no-scripts', '--no-chown',
            'add'] + files, check=True)
        # Keep Alpine musl/libstdc++/Qt together. Overlay only the 9os test boot,
        # accounts and smoke programs, not any source-built shared libraries.
        shutil.copytree(TOP / 'userland/skel', stage, dirs_exist_ok=True, symlinks=True)
        shutil.copytree(TOP / 'userland/alpine/overlay', stage, dirs_exist_ok=True, symlinks=True)
        os.chmod(stage / 'etc/shadow', 0o640)
        os.chmod(stage / 'bin/busybox', 0o4755)
        manifest = {'alpine': VERSION, 'arch': arch, 'minirootfs_sha256': MINI[arch], 'packages': entries}
        text = json.dumps(manifest, indent=2) + '\n'
        (stage / 'etc/9os-alpine.json').write_text(text)
        (work / f'alpine-{arch}.lock.json').write_text(text)
        # mtime-independent image identity also invalidates the Make initramfs.
        (stage / '.9os-image-id').write_text(hashlib.sha256(text.encode()).hexdigest() + '\n')
        if root.exists():
            shutil.rmtree(root)
        stage.rename(root)
    print(f'Alpine {arch}: {len(entries)} signed APKs, no target compilation, {time.monotonic()-start:.1f}s')
    print(f'Image: {root}; lock: {work}/alpine-{arch}.lock.json')

if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as e:
        raise SystemExit(f'Alpine build subprocess exited {e.returncode}; see diagnostics above') from None
