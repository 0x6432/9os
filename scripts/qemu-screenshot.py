#!/usr/bin/env python3
"""Capture the physical QEMU display, never a substitute for guest input tests."""
import pathlib,socket,sys
from PIL import Image
sock,path=sys.argv[1:]
path=pathlib.Path(path).resolve()
if any(c.isspace() for c in str(path)):
    raise SystemExit('Screenshot path must not contain whitespace')
ppm=path.with_suffix('.ppm')
# A fresh file is mandatory: a failed monitor command must not reuse old evidence.
ppm.unlink(missing_ok=True);path.unlink(missing_ok=True)
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
    im.save(path)
    print(f'PASS: physical QEMU display captured {im.width}x{im.height}; input requires separate PTY proof')
ppm.unlink()
