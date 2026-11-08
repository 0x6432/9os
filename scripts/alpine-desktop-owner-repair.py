#!/usr/bin/env python3
"""Repair TWO verified x86 Alpine desktop image groups, never boot a guest.

Only a disposable unmounted mkdisk .part is eligible. This is not a complete
filesystem policy, capability implementation, or security approval.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import tempfile
import tarfile

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'userland/alpine/locks/x86_64-desktop.json'
LOCK_SHA = '4d4953b7185d9915bd8681b0c850c1186588b3fbb95421ec3f4e659b891380cb'
POLICY = (
    ('dbus-daemon-launch-helper-1.16.2-r1.apk', '8845aa8f7eb24b39cc91b9053bbb81c762f512a5ee4320a13f0c2e2bf06f4267',
     'usr/libexec/dbus-daemon-launch-helper', 'messagebus', 81, 0o4750),
    ('linux-pam-1.7.0-r4.apk', '0152e9cd1d13986b75e4ccec8155a878791083ddca7e2667bbbb398bc62c9703',
     'usr/sbin/unix_chkpwd', 'shadow', 42, 0o2755),
)


def require(ok, reason):
    if not ok: raise ValueError(reason)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024*1024), b''): h.update(chunk)
    return h.hexdigest()


def verified_contract(cache, work):
    require(digest(LOCK) == LOCK_SHA, 'desktop lock differs from reviewed x86 policy')
    lock = json.loads(LOCK.read_text())
    require(lock.get('arch') == 'x86_64' and lock.get('profile') == 'desktop', 'wrong desktop lock')
    files = []
    for name, sha, path, group, gid, mode in POLICY:
        matching = [e for e in lock['packages'] if e['url'].endswith('/'+name)]
        require(len(matching) == 1 and matching[0]['sha256'] == sha, 'policy APK not uniquely pinned')
        p = cache / name
        require(p.is_file() and not p.is_symlink() and digest(p) == sha, 'missing or mismatched policy APK: '+name)
        files.append(p)
    spec = importlib.util.spec_from_file_location('oracle_packages', ROOT/'scripts/evdev-linux-oracle-packages.py')
    oracle = importlib.util.module_from_spec(spec);spec.loader.exec_module(oracle)
    oracle_lock = oracle.load_lock(oracle.LOCK)
    verifier = oracle.bootstrap(oracle_lock, cache, work, True, oracle.checked)
    oracle.checked(verifier + ['verify'] + [str(p) for p in files])
    contracts = []
    for p, (_, _, name, group, gid, mode) in zip(files, POLICY):
        # Parse content only after checksum and signature verification.
        with tarfile.open(p, 'r:gz', ignore_zeros=True) as archive:
            members = [m for m in archive if m.name == name]
            require(len(members) == 1 and members[0].isfile() and 0 < members[0].size <= 1024*1024,
                    'missing, ambiguous or unsafe signed file: '+name)
            m = members[0]
            require((m.uname, m.gname, m.mode) == ('root', group, mode), 'signed ownership/mode changed: '+name)
            raw = archive.extractfile(m).read()
        contracts.append(('/'+name, gid, mode, hashlib.sha256(raw).hexdigest()))
    return contracts


def debugfs(image, command, writable=False):
    p = subprocess.run(['debugfs'] + (['-w'] if writable else []) + ['-R', command, str(image)],
                       capture_output=True, text=True, timeout=30, env={**os.environ, 'LC_ALL':'C'})
    require(p.returncode == 0, 'debugfs failed: '+command)
    # debugfs sometimes reports errors while returning status 0.
    require(not re.search(r'(?i)file not found|not found by ext2|error|invalid|read-only|failed|usage:', p.stderr),
            'debugfs error: '+command+': '+p.stderr.strip())
    return p.stdout


def inode(image, path):
    out=debugfs(image, 'stat '+path)
    a=re.search(r'Inode:\s*(\d+)\s+Type:\s*(\w+)\s+Mode:\s*([0-7]+)', out)
    b=re.search(r'User:\s*(\d+)\s+Group:\s*(\d+)', out)
    c=re.search(r'Links:\s*(\d+)', out)
    require(a is not None and b is not None and c is not None, 'unreadable inode: '+path)
    return int(a[1]),a[2],int(a[3],8),int(b[1]),int(b[2]),int(c[1])


def safe_parents(image, path):
    parts=Path(path).parts
    for i in range(2,len(parts)):
        parent='/'+('/'.join(parts[1:i]))
        require(inode(image,parent)[1]=='directory', 'non-directory or symlink parent: '+parent)


def validate_groups(image):
    require(inode(image,'/etc/group')[1]=='regular', 'group file is not regular')
    text=debugfs(image,'cat /etc/group')
    records=[line.split(':') for line in text.splitlines() if line]
    for name,gid in [('messagebus','81'),('shadow','42')]:
        matches=[r for r in records if len(r)>=3 and r[0]==name]
        require(len(matches)==1 and matches[0][2]==gid, 'missing/incorrect group identity: '+name)
        require(sum(1 for r in records if len(r)>=3 and r[2]==gid)==1, 'ambiguous group id: '+gid)


def repair(image, contracts, work):
    st=image.lstat()
    require(stat.S_ISREG(st.st_mode) and st.st_nlink==1 and image.name.endswith('.part'),
            'requires an unshared regular temporary .part image')
    safe_parents(image,'/etc/group');validate_groups(image)
    observed=[]
    for i,(path,gid,mode,sha) in enumerate(contracts):
        safe_parents(image,path)
        ino,kind,actual_mode,uid,actual_gid,links=inode(image,path)
        require(kind=='regular' and links==1 and uid==0 and actual_mode==mode and actual_gid in (0,gid),
                'unsafe owner, mode, type, link count or group: '+path)
        out=work/('extracted-'+str(i))
        debugfs(image,'dump '+path+' '+str(out))
        require(out.is_file() and digest(out)==sha, 'disk payload differs from signed APK: '+path)
        observed.append((path,ino,gid,mode,sha))
    require(len({v[1] for v in observed})==len(observed), 'policy files alias one inode')
    # All validations precede either mutation. The caller must not publish a
    # .part after any error; changes are re-read and payloads rechecked below.
    for path,ino,gid,mode,sha in observed:
        debugfs(image, 'set_inode_field '+path+' gid '+str(gid), True)
    for i,(path,ino,gid,mode,sha) in enumerate(observed):
        require(inode(image,path)==(ino,'regular',mode,0,gid,1), 'post-write metadata mismatch: '+path)
        out=work/('post-'+str(i));debugfs(image,'dump '+path+' '+str(out))
        require(out.is_file() and digest(out)==sha, 'post-write payload mismatch: '+path)
    return observed


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('partition', type=Path)
    ap.add_argument('--cache', type=Path, required=True, help='offline pinned mini and two signed APKs')
    args=ap.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix='alpine-owner-repair-') as tmp:
            work=Path(tmp); contracts=verified_contract(args.cache,work);result=repair(args.partition,contracts,work)
        for path,ino,gid,mode,sha in result:print('VERIFIED',path,'uid=0 gid='+str(gid),'mode='+oct(mode),'sha256='+sha)
        print('Two group owners only; NOT a complete image security verification.')
    except (OSError, ValueError, subprocess.SubprocessError, tarfile.TarError) as e:
        ap.exit(1,'ownership repair refused: '+str(e)+'\n')

if __name__=='__main__':main()
