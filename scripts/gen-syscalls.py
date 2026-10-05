#!/usr/bin/env python3
"""Regenerate kernel/core/syscall.c from arch/<ARCH>/include/arch/unistd.h and the sys_* functions."""
import re, glob, sys, os
os.chdir(os.path.join(os.path.dirname(__file__), '..', 'kernel'))
arch = sys.argv[1] if len(sys.argv) > 1 else 'x86_64'
defs = set()
for f in glob.glob('core/sys_*.c') + ['core/signal.c', 'fs/anonfd.c'] + glob.glob('net/*.c') + glob.glob('arch/*/user.c'):
    defs |= set(re.findall(r'^int64_t (sys_\w+)\(', open(f).read(), re.M))
alias = {'arch_prctl':'sys_arch_prctl_wrap','madvise':'sys_madvise','fadvise64':'sys_zero','fdatasync':'sys_fsync',
         'syncfs':'sys_fsync','msync':'sys_zero','mlock':'sys_zero','munlock':'sys_zero','flock':'sys_zero',
         'capset':'sys_zero','umount2':'sys_zero','get_robust_list':'sys_zero',
         'sched_getparam':'sys_zero','sched_getscheduler':'sys_zero','futimesat':'sys_zero','utime':'sys_zero',
         'utimes':'sys_zero','clock_settime':'sys_zero','fallocate':'sys_zero','rseq':'sys_rseq'}
nrs = []
for a in sorted(glob.glob('arch/*/include/arch/unistd.h')):
    for n, _ in re.findall(r'#define __NR_(\w+) (\d+)', open(a).read()):
        if n not in nrs: nrs.append(n)
ok = lambda fn: fn in defs
rows, names, decls = [], [], set()
for n in nrs:
    fn = alias.get(n, 'sys_' + n)
    names.append(f'#ifdef __NR_{n}\n    [__NR_{n}] = "{n}",\n#endif')
    if not ok(fn): continue
    decls.add(f'int64_t {fn}();')
    rows.append(f'#ifdef __NR_{n}\n    [__NR_{n}] = (syscall_fn){fn},\n#endif')
tmpl = open('core/syscall.c.in').read()
out = tmpl.replace('@DECLS@', '\n'.join(sorted(decls))).replace('@TABLE@', '\n'.join(rows)).replace('@NAMES@', '\n'.join(names))
open('core/syscall.c', 'w').write(out)
print(f'{len(rows)} syscalls wired')
