#!/usr/bin/env python3
"""Build an isolated, signed Alpine runtime; never execute target package scripts."""
import argparse, concurrent.futures, fcntl, hashlib, json, os, pathlib, platform, re
import shutil, subprocess, tempfile, time, urllib.request

VERSION = '3.22.6'
BRANCH = 'v3.22'
BASE = 'https://dl-cdn.alpinelinux.org/alpine'
MINI = {
    'x86_64': '27694aaa55fd7a9e3ef596e0ad4eb66802308bb20172b17030cd5f4d8ae9bac2',
    'aarch64': '821565fa8f3953eefd12497b166b4b50add2f7c57fb312e75862f5867e06fefe',
    'riscv64': '72f7e69f6950c2799137a934fae61513016efa546afcb4a05540a505c4184150',
}
PACKAGES = ['qt6-qtdeclarative', 'dbus', 'font-dejavu', 'sway', 'grim', 'jq', 'strace', 'musl-dev']
PROFILES = {'base': PACKAGES, 'desktop': PACKAGES + ['seatd', 'mesa-dri-gallium', 'kwin', 'plasma-workspace', 'plasma-desktop', 'breeze', 'plasma-workspace-wallpapers', 'kquickcharts', 'kpipewire', 'plasma-pa', 'tzdata', 'konsole', 'dolphin']}
TOP = pathlib.Path(__file__).resolve().parents[2]

def digest(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def download(url, path, sha=None, offline=False):
    if path.exists() and (sha is None or digest(path) == sha):
        return
    if offline:
        raise ValueError('Offline cache missing or checksum mismatch: ' + str(path))
    part = path.with_suffix(path.suffix + '.part')
    failures = []
    for attempt in range(3):
        # Diagnostic clocks must not change acquisition or mask its exception.
        try:
            utc_start = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(time.time()))
        except Exception:
            utc_start = 'unknown'
        try:
            started = time.monotonic()
        except Exception:
            started = None
        try:
            with urllib.request.urlopen(url, timeout=60) as r, open(part, 'wb') as f:
                shutil.copyfileobj(r, f)
            if sha and digest(part) != sha:
                raise ValueError('checksum mismatch: ' + url)
            part.replace(path)
            return
        except Exception as error:
            part.unlink(missing_ok=True)
            if isinstance(error, urllib.error.HTTPError):
                kind = 'HTTPError'
            elif isinstance(error, TimeoutError):
                kind = 'TimeoutError'
            elif isinstance(error, urllib.error.URLError):
                kind = 'URLError'
            elif isinstance(error, OSError):
                kind = 'OSError'
            else:
                kind = 'other'
            status = error.code if kind == 'HTTPError' else None
            try:
                elapsed = round(time.monotonic() - started, 3) if started is not None else 'unknown'
            except Exception:
                elapsed = 'unknown'
            failures.append({'attempt': attempt + 1, 'utc_start': utc_start,
                             'elapsed_s': elapsed,
                             'class': kind,
                             'http_status': status if type(status) is int and 100 <= status <= 599 else 'unknown'})
            if attempt == 2:
                error.add_note('Download failed: ' + url)
                # Only fixed-shape, message-free records reach this bounded note.
                error.add_note(('Download attempts: ' + json.dumps(failures, separators=(',', ':')))[:1024])
                raise
            time.sleep(attempt + 1)

def validate_entries(entries, arch, locked):
    if not isinstance(entries, list) or not entries:
        raise ValueError('Empty/invalid package set')
    pattern = re.compile(re.escape(BASE + '/' + BRANCH) +
                         r'/(main|community)/' + re.escape(arch) + r'/[A-Za-z0-9+_.-]+\.apk')
    names = set()
    for e in entries:
        if not isinstance(e, dict) or not pattern.fullmatch(e.get('url', '')):
            raise ValueError('Package URL must name an APK in the selected stable architecture')
        name = e['url'].rsplit('/', 1)[-1]
        if name in names:
            raise ValueError('Duplicate package filename: ' + name)
        names.add(name)
        sha = e.get('sha256')
        if (locked or sha is not None) and (not isinstance(sha, str) or not re.fullmatch('[0-9a-f]{64}', sha)):
            raise ValueError('Every locked APK needs an exact SHA256')
    return entries

def validate_profile(entries, profile="base"):
    # An older offscreen-only lock must not silently create a broken Wayland image.
    files = [e['url'].rsplit('/', 1)[-1] for e in entries]
    for package in PROFILES[profile]:
        if not any(re.fullmatch(re.escape(package) + r'-[0-9][A-Za-z0-9+_.-]*\.apk', f) for f in files):
            raise ValueError('Lock missing required profile package: ' + package)
    if 'qt6-qtdeclarative-6.8.3-r0.apk' not in files:
        raise ValueError('Expected the reviewed Alpine Qt6.8.3 package set')

def mini(arch, cache, offline=False):
    name = f'alpine-minirootfs-{VERSION}-{arch}.tar.gz'
    path = cache / name
    download(f'{BASE}/{BRANCH}/releases/{arch}/{name}', path, MINI[arch], offline)
    return path

def unpack(archive, root):
    subprocess.run(['tar', 'xzf', str(archive), '--no-same-owner', '-C', str(root)], check=True)

def merge_named_records(original, overlay):
    """Preserve distro account/group names while installing explicit VM users."""
    replacements = {line.split(':', 1)[0]: line for line in overlay.splitlines() if line}
    result = []
    for line in original.splitlines():
        name = line.split(':', 1)[0]
        result.append(replacements.pop(name, line))
    return '\n'.join(result + list(replacements.values())) + '\n'

def install_desktop_accounts(stage):
    # --no-scripts deliberately skips dbus.pre-install. Provision only this
    # explicitly required, locked-down VM daemon identity, not arbitrary hooks.
    records = {
        'passwd': 'messagebus:x:81:81:DBus system bus:/dev/null:/sbin/nologin',
        'group': 'messagebus:x:81:',
        'shadow': 'messagebus:!:0:0:99999:7:::',
    }
    for name, record in records.items():
        path = stage / 'etc' / name
        original = path.read_text()
        lines = [line.split(':') for line in original.splitlines() if line]
        existing = [line for line in lines if line[0] == 'messagebus']
        if name in ('passwd', 'group') and any(line[0] != 'messagebus' and line[2] == '81' for line in lines):
            raise ValueError('VM messagebus id81 already in use')
        if existing:
            if len(existing) != 1:
                raise ValueError('Duplicate VM messagebus identity')
            row = existing[0]
            if name == 'passwd' and (len(row) != 7 or row[2:4] != ['81', '81'] or row[6] not in ('/sbin/nologin', '/bin/false')):
                raise ValueError('messagebus must use the unprivileged nologin VM identity')
            if name == 'group' and (len(row) != 4 or row[2] != '81'):
                raise ValueError('messagebus must use the unprivileged VM group')
            if name == 'shadow' and (len(row) != 9 or not row[1].startswith(('!', '*'))):
                raise ValueError('messagebus password must be locked')
            continue
        path.write_text(original.rstrip('\n') + '\n' + record + '\n')

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--arch', choices=MINI, default=os.environ.get('ARCH', 'x86_64'))
    ap.add_argument('--profile', choices=PROFILES, default='base', help='Isolated base fast lane or larger KDE desktop runtime')
    ap.add_argument('--lock', type=pathlib.Path, help='Replay exact signed APK URLs/hashes; no index refresh')
    ap.add_argument('--offline', action='store_true', help='Require locked, cached artifacts; never contact repositories')
    ap.add_argument('--resolve', action='store_true', help='Explicitly refresh dependency resolution instead of a checked-in lock')
    args = ap.parse_args()
    suffix = '' if args.profile == 'base' else '-desktop'
    image = 'alpine' + suffix
    checked = TOP / f'userland/alpine/locks/{args.arch}{suffix}.json'
    if args.resolve and args.lock:
        ap.error('--resolve and --lock are mutually exclusive')
    if args.lock is None and not args.resolve and checked.exists():
        args.lock = checked
    if args.offline and not args.lock:
        ap.error('--offline requires a checked-in or explicit lock')
    if platform.machine() != 'x86_64':
        ap.error('bootstrap currently requires an x86_64 build host (all targets cross-installed)')
    start = time.monotonic()
    arch = args.arch
    cache = pathlib.Path(os.environ.get('TOOLS_DIR', '/data/tools')).resolve() / 'dl' / 'alpine' / arch
    cache.mkdir(parents=True, exist_ok=True)
    work = TOP / 'userland/build'
    work.mkdir(exist_ok=True)
    guard = open(work / f'{image}-{arch}.build.lock', 'w')
    fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
    root = TOP / f'userland/{image}-root-{arch}'
    if root.exists() and not (root / 'etc/9os-alpine.json').is_file():
        raise SystemExit('Refusing to replace an unmarked image: ' + str(root))
    with tempfile.TemporaryDirectory(prefix=f'alpine-{arch}-', dir=work) as tmp:
        tmp = pathlib.Path(tmp)
        host = tmp / 'host'; host.mkdir()
        unpack(mini('x86_64', cache, args.offline), host)
        stage = tmp / 'root'; stage.mkdir()
        unpack(mini(arch, cache, args.offline), stage)
        repos = tmp / 'repositories'
        repos.write_text('\n'.join(f'{BASE}/{BRANCH}/{r}' for r in ['main', 'community']) + '\n')
        apk = [str(host / 'lib/ld-musl-x86_64.so.1'), '--library-path',
               f'{host}/lib:{host}/usr/lib', str(host / 'sbin/apk')]
        # Repository signing keys differ by architecture. The TARGET mini is
        # independently SHA-pinned too; never disable verification to cross-install.
        trust = ['--keys-dir', str(stage / 'etc/apk/keys')]
        if args.lock:
            lock = json.loads(args.lock.read_text())
            if (lock['arch'], lock['alpine'], lock['minirootfs_sha256']) != (arch, VERSION, MINI[arch]):
                raise ValueError('lock architecture/release/bootstrap mismatch')
            if lock.get('profile', 'base') != args.profile:
                raise ValueError('lock profile mismatch')
            entries = lock['packages']
        else:
            cmd = apk + ['--root', str(host), '--arch', arch] + trust + [
                '--repositories-file', str(repos), '--no-cache', 'fetch', '--simulate', '--recursive', '--url'] + PROFILES[args.profile]
            # --simulate is essential: apk2 --url alone downloads too, and omits
            # cached files from its URL output. Resolve from an empty directory.
            out = subprocess.check_output(cmd, text=True, cwd=tmp)
            entries = [{'url': u, 'sha256': None} for u in out.splitlines() if u.startswith('https://') and u.endswith('.apk')]
            if not entries or not any(u['url'].endswith('/qt6-qtdeclarative-6.8.3-r0.apk') for u in entries):
                raise ValueError('Expected the reviewed Alpine Qt6.8.3 package set')
        validate_entries(entries, arch, locked=bool(args.lock))
        validate_profile(entries, args.profile)
        def fetch(entry):
            url = entry['url']
            path = cache / url.rsplit('/', 1)[-1]
            download(url, path, entry['sha256'], args.offline)
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
        distro_accounts = {name: (stage / 'etc' / name).read_text()
                           for name in ['passwd', 'group', 'shadow']}
        shutil.copytree(TOP / 'userland/skel', stage, dirs_exist_ok=True, symlinks=True)
        for name, original in distro_accounts.items():
            path = stage / 'etc' / name
            path.write_text(merge_named_records(original, path.read_text()))
        shutil.copytree(TOP / 'userland/alpine/overlay', stage, dirs_exist_ok=True, symlinks=True)
        if args.profile == 'desktop':
            shutil.copytree(TOP / 'userland/alpine/desktop-overlay', stage, dirs_exist_ok=True, symlinks=True)
        # --no-scripts is intentional. Generate only these known declarative
        # caches with HOST tools, never arbitrary APK post-install code or a
        # target executable (including on aarch64/riscv64).
        if args.profile == 'desktop':
            for tool, path, options in [
                ('glib-compile-schemas', 'usr/share/glib-2.0/schemas', ['--strict']),
                ('update-mime-database', 'usr/share/mime', []),
            ]:
                executable = shutil.which(tool)
                if executable is None:
                    raise ValueError('Desktop profile requires host tool: ' + tool)
                subprocess.run([executable, *options, str(stage / path)], check=True)
        if args.profile == 'desktop':
            install_desktop_accounts(stage)
            shutil.copyfile(stage / 'usr/share/zoneinfo/UTC', stage / 'etc/localtime')
            (stage / 'etc/timezone').write_text('UTC\n')
        os.chmod(stage / 'etc/shadow', 0o640)
        os.chmod(stage / 'bin/busybox', 0o4755)
        manifest = {'profile': args.profile, 'alpine': VERSION, 'arch': arch, 'minirootfs_sha256': MINI[arch], 'packages': entries}
        if args.profile == 'desktop':
            manifest['generated_caches'] = {
                path: digest(stage / path) for path in [
                    'usr/share/glib-2.0/schemas/gschemas.compiled',
                    'usr/share/mime/mime.cache',
                ]
            }
        text = json.dumps(manifest, indent=2) + '\n'
        (stage / 'etc/9os-alpine.json').write_text(text)
        (work / f'{image}-{arch}.lock.json').write_text(text)
        # mtime-independent image identity also invalidates the Make initramfs.
        (stage / '.9os-image-id').write_text(hashlib.sha256(text.encode()).hexdigest() + '\n')
        if root.exists():
            shutil.rmtree(root)
        stage.rename(root)
    print(f'Alpine {args.profile}/{arch}: {len(entries)} signed APKs, no target compilation, {time.monotonic()-start:.1f}s')
    print(f'Image: {root}; lock: {work}/{image}-{arch}.lock.json')

if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as e:
        raise SystemExit(f'Alpine build subprocess exited {e.returncode}; see diagnostics above') from None
