#!/usr/bin/env python3
"""Tiny x86 native reference and exact-production syscall model, no guest/build."""
from pathlib import Path
import argparse, hashlib, os, platform, subprocess, tempfile
p=argparse.ArgumentParser()
p.add_argument('--mode',choices=['native','model'],required=True)
p.add_argument('--source',type=Path,help='preserved full syscall-section snapshot for baseline')
p.add_argument('--sanitize',action='store_true',help='UBSan on tiny consumer/model only')
p.add_argument('--mutations',action='store_true',help='require each named model regression to fail')
a=p.parse_args()
if platform.machine() != 'x86_64': raise SystemExit('x86_64 host only; no non-x86 validation claimed')
if a.mutations and a.mode!='model': raise SystemExit('mutations are model-only')
d=Path(__file__).resolve().parent
root=d.parents[1]
print('host:',platform.platform(),flush=True)
print('compiler:',subprocess.check_output([os.environ.get('CC','cc'),'--version'],text=True).splitlines()[0],flush=True)
variants={'normal':None}
if a.mode=='model':
    text=(a.source or root/'kernel/fs/anonfd.c').read_text()
    section=text[text.index('static void ep_set_mask'):text.index('/* ------------------------------------------------------------------ memfd')]
    print('extracted syscall sha256:',hashlib.sha256(section.encode()).hexdigest(),flush=True)
    variants['normal']=section
    if a.mutations:
        mutations={
            'ignore-sigset-size':('if (sz != sizeof m) return -EINVAL;', '(void)sz;'),
            'accept-billion-ns':('ts.tv_nsec >= 1000000000','ts.tv_nsec > 1000000000'),
            'truncate-to-milliseconds':('return ep_pwait(epfd, uev, max, ns, usig, sz);','return ep_pwait(epfd, uev, max, ns == UINT64_MAX ? ns : ns / 1000000 * 1000000, usig, sz);'),
            'overflow-seconds':('? INT64_MAX : (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;', '? (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec : (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;'),
            'lose-original-deadline':('deadline - now,','timeout_ns,'),
            'lose-filter':('ep_wait_filter(&x));','UINT64_MAX);'),
            'restore-mask-too-early':('if (r == -EINTR) { current->saved_mask = oldmask; current->restore_mask = true; }','if (r == -EINTR) { ep_set_mask(oldmask); }'),
        }
        for name,(old,new) in mutations.items():
            if old not in section: raise SystemExit('mutation anchor missing: '+name)
            variants[name]=section.replace(old,new)
with tempfile.TemporaryDirectory(prefix='9os-epoll-abi-') as tmp:
    for name,body in variants.items():
        src=d/'native.c'
        if a.mode=='model':
            src=Path(tmp)/(name+'.c')
            src.write_text((d/'model-pre.c').read_text()+body+(d/'model-post.c').read_text())
        exe=Path(tmp)/name
        cmd=[os.environ.get('CC','cc'),'-std=gnu11','-O2','-Wall','-Wextra','-Werror',
             '-Wno-unused-parameter','-I'+str(d),str(src),'-o',str(exe)]
        if a.sanitize: cmd+=['-fsanitize=undefined','-fno-sanitize-recover=all']
        print('compile:', ' '.join(cmd).replace(tmp,'<tmp>'),flush=True)
        subprocess.run(cmd,check=True)
        r=subprocess.run([str(exe)],timeout=5,capture_output=name!='normal',text=True)
        if name=='normal':
            if r.returncode: raise SystemExit(r.returncode)
        else:
            if r.returncode!=1 or 'FAIL ' not in r.stdout:
                raise SystemExit(f'{name}: expected checked mismatch, got {r.returncode}\n{r.stderr}')
            failed=[line for line in r.stdout.splitlines() if line.startswith('FAIL ')]
            print(f'rejected {name}: {len(failed)} mismatches; first: {failed[0]}',flush=True)
