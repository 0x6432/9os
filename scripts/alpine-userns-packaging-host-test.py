#!/usr/bin/env python3
"""Offline host-only tiny image control. Not fixture approval or a guest test.

Provide a cache containing the pinned mini and three exact signed APKs:
  python3 scripts/alpine-userns-packaging-host-test.py CACHE
Requires unprivileged user/network namespaces, sfdisk, e2fsprogs and setcap.
Do not run QEMU in the namespace; this test never starts a guest.
"""
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
WRAPPER = 'busybox-suid-1.37.0-r20.apk'
WRAPPER_SHA = 'f139a0f28d40a3a183661ee1ad7a248fcf01e82b144114f86d3bc16cee990917'
BBSUID_SHA = 'a5b65fb3dee405be3c7a6f8d219ab9a432bda4813cf6a8b98279c2a12a454533'

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module

def call(argv, **kwargs):
    return subprocess.run([str(x) for x in argv], check=True, **kwargs)

def read_member(archive, name):
    with tarfile.open(archive, 'r:gz', ignore_zeros=True) as t:
        matches = [m for m in t if m.name == name]
        if len(matches) != 1 or not matches[0].isfile():
            raise ValueError('missing or ambiguous regular archive member: ' + name)
        return matches[0], t.extractfile(matches[0]).read()

def inner(cache, work):
    mapping = Path('/proc/self/uid_map').read_text().split()
    if os.geteuid() != 0 or len(mapping) != 3 or mapping[0] != '0' or mapping[1] == '0' or mapping[2] != '1':
        raise ValueError('requires a one-ID non-root user namespace')
    owner = load('owner_repair', 'scripts/alpine-desktop-owner-repair.py')
    oracle = load('oracle_packages', 'scripts/evdev-linux-oracle-packages.py')
    cache = Path(cache).resolve(); work = Path(work).resolve()
    contracts = owner.verified_contract(cache, work)
    wrapper = cache / WRAPPER
    if wrapper.is_symlink() or owner.digest(wrapper) != WRAPPER_SHA:
        raise ValueError('wrong signed wrapper APK')
    verify_work = work / 'wrapper-verifier'; verify_work.mkdir()
    verifier = oracle.bootstrap(oracle.load_lock(oracle.LOCK), cache, verify_work, True, oracle.checked)
    oracle.checked(verifier + ['verify', str(wrapper)])
    stage = work / 'root'; stage.mkdir()
    def put(name, raw, mode):
        p = stage / name; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(raw); p.chmod(mode)
    for apk, _, name, _, _, mode in owner.POLICY:
        m, raw = read_member(cache / apk, name)
        if m.mode != mode: raise ValueError('unexpected signed helper mode')
        put(name, raw, mode)
    mini = cache / 'alpine-minirootfs-3.22.6-x86_64.tar.gz'
    _, busybox = read_member(mini, './bin/busybox')
    m, bbsuid = read_member(wrapper, 'bin/bbsuid')
    if m.mode != 0o4111 or hashlib.sha256(bbsuid).hexdigest() != BBSUID_SHA:
        raise ValueError('unexpected signed wrapper contents or mode')
    put('bin/busybox', busybox, 0o755); put('bin/bbsuid', bbsuid, 0o4111)
    put('etc/group', b'root:x:0:\nmessagebus:x:81:\nshadow:x:42:\n', 0o644)
    put('bin/captest', b'not executable\n', 0o755)
    call(['setcap', 'cap_net_raw+ep', stage/'bin/captest'])
    disk = work / 'disk.raw'; part = work / 'partition.ext2'
    env = dict(os.environ, ALPINE_DESKTOP_OWNER_POLICY='x86_64', ALPINE_DESKTOP_APK_CACHE=str(cache), INODE_SIZE='256')
    call([ROOT/'scripts/mkdisk.sh', disk, '64', stage], cwd=ROOT, env=env)
    with disk.open('rb') as src, part.open('wb') as dst:
        src.seek(1024*1024); dst.write(src.read(62*1024*1024))
    for path, gid, mode, sha in contracts:
        info = owner.inode(part, path)
        if info[1:] != ('regular', mode, 0, gid, 1): raise ValueError('bad repaired inode: ' + path)
    for path, mode in [('/bin/busybox', 0o755), ('/bin/bbsuid', 0o4111)]:
        info = owner.inode(part, path)
        if info[1:] != ('regular', mode, 0, 0, 1): raise ValueError('bad inode: ' + path)
    extracted = work / 'bbsuid'; owner.debugfs(part, 'dump /bin/bbsuid ' + str(extracted))
    if owner.digest(extracted) != BBSUID_SHA: raise ValueError('wrapper bytes changed')
    cap = owner.debugfs(part, 'ea_get /bin/captest security.capability')
    if '01 00 00 02 00 20 00 00 00 00 00 00 00 00 00 00 00 00 00 00' not in cap:
        raise ValueError('synthetic capability did not survive as expected')
    call(['e2fsck', '-fn', part], stdout=subprocess.DEVNULL)
    print('PASS: signed helper group repair and wrapper; synthetic capability preserved in tiny ext2. No guest boot.', flush=True)

def main():
    if len(sys.argv) == 4 and sys.argv[1] == '--inner':
        inner(sys.argv[2], sys.argv[3]); return
    if len(sys.argv) != 2: raise SystemExit(__doc__)
    if os.geteuid() == 0: raise SystemExit('refusing to run as host root')
    cache = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='m40-userns-package-') as temp:
        call(['unshare', '-Urn', sys.executable, Path(__file__).resolve(), '--inner', cache, temp])

if __name__ == '__main__': main()
