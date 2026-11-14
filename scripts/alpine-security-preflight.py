#!/usr/bin/env python3
"""Check a few known x86 stage invariants; NOT a complete security review."""
import argparse, hashlib, os, pathlib, stat, sys
BASE_MODES = {'bin/busybox':0o755,'bin/bbsuid':0o4111,
              'usr/libexec/dbus-daemon-launch-helper':0o4750}
DESKTOP_MODES = {'usr/sbin/unix_chkpwd':0o2755,
                 **{'usr/bin/'+n:0o4755 for n in ('chage','chfn','chsh','expiry','gpasswd','passwd','fusermount3')}}
HASHES = {'bin/busybox':'6dcd5f0190e8f4b055c2823b3d97660704b97d70c60cd3d3cf9805922fde1302',
          'bin/bbsuid':'a5b65fb3dee405be3c7a6f8d219ab9a432bda4813cf6a8b98279c2a12a454533'}
CAPS = {'usr/bin/sway':bytes.fromhex('0100000200008000000000000000000000000000')}
DESKTOP_CAPS = {'usr/libexec/gstreamer-1.0/gst-ptp-helper':bytes.fromhex('0100000200148000000000000000000000000000')}

def inspect(root, profile):
    problems=[]
    modes=dict(BASE_MODES);caps=dict(CAPS)
    if profile=='desktop':modes.update(DESKTOP_MODES);caps.update(DESKTOP_CAPS)
    for name,expected in modes.items():
        p=root/name
        try:
            info=p.lstat()
            if not stat.S_ISREG(info.st_mode) or stat.S_IMODE(info.st_mode)!=expected:
                problems.append(name+': wrong type or mode');continue
            if name in HASHES and hashlib.sha256(p.read_bytes()).hexdigest()!=HASHES[name]:
                problems.append(name+': wrong content hash')
        except OSError as e:problems.append(name+': '+str(e))
    for name,expected in caps.items():
        try:
            p=root/name
            if not stat.S_ISREG(p.lstat().st_mode) or os.getxattr(p,'security.capability',follow_symlinks=False)!=expected:
                problems.append(name+': wrong type or capability')
        except OSError as e:problems.append(name+': '+str(e))
    for name,target in [('bin/su','/bin/bbsuid'),('bin/sh','/bin/busybox')]:
        try:
            if os.readlink(root/name)!=target:problems.append(name+': wrong symlink target')
        except OSError as e:problems.append(name+': '+str(e))
    return problems

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=pathlib.Path);p.add_argument('profile',choices=['base','desktop']);a=p.parse_args()
    errors=inspect(a.root,a.profile)
    for error in errors:print('FAIL '+error,file=sys.stderr)
    if errors:return 1
    print('PASS narrow x86 stage invariants only; not complete security verification')
    return 0
if __name__=='__main__':raise SystemExit(main())
