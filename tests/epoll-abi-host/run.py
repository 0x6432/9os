#!/usr/bin/env python3
"""Tiny x86 native reference and exact-production syscall model, no guest/build."""
from pathlib import Path
import argparse, hashlib, os, platform, subprocess, tempfile
p=argparse.ArgumentParser()
p.add_argument('--mode',choices=['native','model'],required=True)
p.add_argument('--source',type=Path,help='preserved full syscall-section snapshot for baseline')
a=p.parse_args()
if platform.machine() != 'x86_64': raise SystemExit('x86_64 host only; no non-x86 validation claimed')
d=Path(__file__).resolve().parent
root=d.parents[1]
print('host:',platform.platform(),flush=True)
with tempfile.TemporaryDirectory(prefix='9os-epoll-abi-') as tmp:
    src=d/'native.c'
    if a.mode=='model':
        text=(a.source or root/'kernel/fs/anonfd.c').read_text()
        section=text[text.index('static void ep_set_mask'):text.index('/* ------------------------------------------------------------------ memfd')]
        print('extracted syscall sha256:',hashlib.sha256(section.encode()).hexdigest(),flush=True)
        src=Path(tmp)/'model.c'
        src.write_text((d/'model-pre.c').read_text()+section+(d/'model-post.c').read_text())
    exe=Path(tmp)/a.mode
    cmd=[os.environ.get('CC','cc'),'-std=gnu11','-O2','-Wall','-Wextra','-Werror',
         '-Wno-unused-parameter','-I'+str(d),str(src),'-o',str(exe)]
    print('compile:', ' '.join(cmd).replace(tmp,'<tmp>'),flush=True)
    subprocess.run(cmd,check=True)
    r=subprocess.run([str(exe)],timeout=5)
    raise SystemExit(r.returncode)
