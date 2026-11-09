#!/usr/bin/env python3
"""Capture the physical QEMU display, never a substitute for guest input tests."""
import os,pathlib,socket,sys,tempfile
from PIL import Image
sock,name=sys.argv[1:]
path=pathlib.Path(os.path.abspath(name))
if any(c.isspace() for c in str(path)):
    raise SystemExit('Screenshot path must not contain whitespace')
# Refuse existing files and symlinks rather than deleting or following them.
# Exclusive creation below repeats the check at the actual output open.
if path.exists() or path.is_symlink():
    raise SystemExit('Screenshot destination must be fresh')
with tempfile.TemporaryDirectory(prefix='.9os-screenshot-',dir=path.parent) as directory:
    ppm=pathlib.Path(directory)/'capture.ppm'
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(20);s.connect(sock)
        def prompt():
            data=b''
            while not data.endswith(b'(qemu) '):
                part=s.recv(65536)
                if not part:raise RuntimeError('QEMU monitor closed before prompt')
                data+=part
                if len(data)>1<<20:raise RuntimeError('Unbounded QEMU monitor response')
            return data
        prompt();s.sendall(f'screendump {ppm}\n'.encode());prompt()
    with Image.open(ppm) as im:
        im.load();im=im.convert('RGB')
        if im.width<640 or im.height<480 or len(im.getcolors(im.width*im.height) or [])<1000:
            raise SystemExit('Physical display absent/blank; refusing screenshot evidence')
        fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
        with os.fdopen(fd,'wb') as output:im.save(output,format='PNG')
        print(f'PASS: physical QEMU display captured {im.width}x{im.height}; input requires separate PTY proof')
