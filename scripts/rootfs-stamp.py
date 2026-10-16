#!/usr/bin/env python3
"""Update a Make rootfs stamp from metadata, including whitespace names/symlinks.

Like ordinary Make prerequisites this tracks metadata, not every file's bytes.
ctime also catches content/mode changes with a preserved mtime. Ownership is
omitted because initramfs packaging deliberately normalizes it to root:root.
"""
import hashlib, os, pathlib, sys

def update(root, stamp):
    root=pathlib.Path(root);stamp=pathlib.Path(stamp)
    if not root.is_dir():raise ValueError('Root filesystem directory missing: '+str(root))
    digest=hashlib.sha256()
    for directory, dirs, files in os.walk(root, followlinks=False):
        dirs.sort();files.sort()
        for name in ['.']+dirs+files:
            p=pathlib.Path(directory)/name;st=p.lstat()
            digest.update(os.fsencode(str(p.relative_to(root)))+b'\0')
            digest.update(f'{st.st_mode}:{st.st_size}:{st.st_mtime_ns}:{st.st_ctime_ns}'.encode()+b'\0')
            if p.is_symlink():digest.update(os.fsencode(os.readlink(p))+b'\0')
    text=digest.hexdigest()+'\n'
    if stamp.exists() and stamp.read_text()==text:return False
    stamp.parent.mkdir(parents=True,exist_ok=True)
    temp=stamp.with_name(stamp.name+'.tmp');temp.write_text(text);temp.replace(stamp)
    return True

if __name__=='__main__':
    try:update(sys.argv[1],sys.argv[2])
    except (OSError,ValueError) as e:raise SystemExit(str(e))
