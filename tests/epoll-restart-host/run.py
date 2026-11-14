#!/usr/bin/env python3
"""Compile the exact no_restart function for a tiny x86 policy control only.

The legacy-absent variant undefines one macro while still compiling/executing
native x86 code. It is NOT a non-x86 build or a guest signal-frame test.
"""
from pathlib import Path
import argparse
import hashlib
import os
import platform
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, help='alternate full signal.c for original-policy negative control')
p.add_argument('--mutations', action='store_true')
a = p.parse_args()
if platform.machine() != 'x86_64':
    raise SystemExit('tiny host control is x86_64 only; no non-x86 validation claimed')
d = Path(__file__).resolve().parent
root = d.parents[1]
source = (a.source or root/'kernel/core/signal.c').read_text()
start = source.index('static bool no_restart(uint64_t nr) {')
end = source.index('\n}\n', start)+3
function = source[start:end]
print('host:', platform.platform(), flush=True)
print('compiler:', subprocess.check_output([os.environ.get('CC', 'cc'), '--version'], text=True).splitlines()[0], flush=True)
print('exact no_restart sha256:', hashlib.sha256(function.encode()).hexdigest(), flush=True)
variants = {'normal': function}
if a.mutations:
    replacements = {
        'restore-original-policy': (' ||\n           nr == __NR_epoll_pwait || nr == __NR_epoll_pwait2\n#ifdef __NR_epoll_wait\n           || nr == __NR_epoll_wait\n#endif', ''),
        'omit-epoll-wait': ('           || nr == __NR_epoll_wait\n', ''),
        'omit-epoll-pwait': ('nr == __NR_epoll_pwait || ', ''),
        'omit-epoll-pwait2': (' || nr == __NR_epoll_pwait2', ''),
        'restart-ppoll-regression': (' || nr == __NR_ppoll', ''),
        'change-ordinary-read-policy': ('return nr == __NR_nanosleep', 'return nr == __NR_read || nr == __NR_nanosleep'),
    }
    for name, (old, new) in replacements.items():
        if old not in function:
            raise SystemExit('missing exact mutation anchor: '+name)
        variants[name] = function.replace(old, new, 1)

harness = r'''
static unsigned checks, failures;
static void check(unsigned nr, bool got, bool want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL nr=%u no_restart=%d expected=%d\n", nr, got, want);
    }
}
int main(void) {
    static const unsigned excluded[] = {
        __NR_nanosleep, __NR_clock_nanosleep, __NR_ppoll, __NR_pselect6,
        __NR_rt_sigsuspend, __NR_restart_syscall, __NR_epoll_pwait, __NR_epoll_pwait2,
#ifdef __NR_epoll_wait
        __NR_epoll_wait,
#endif
#ifdef __NR_poll
        __NR_poll, __NR_select, __NR_pause,
#endif
    };
    /* Check every table entry plus one invalid number: reject accidental policy
     * broadening as well as missing epoll/existing exclusions. No signal-frame
     * behavior is reimplemented or claimed by this classification test. */
    for (unsigned nr = 0; nr <= NR_SYSCALLS; nr++) {
        bool want=false;
        for (unsigned i=0; i<sizeof excluded/sizeof *excluded; i++)
            if (nr == excluded[i]) want=true;
        check(nr, no_restart(nr), want);
    }
    printf("exact no_restart host policy: %u checks, %u failures (classification only)\n", checks, failures);
    return failures != 0;
}
'''
with tempfile.TemporaryDirectory(prefix='9os-epoll-restart-') as tmp:
    for name, body in variants.items():
        for legacy in [True, False]:
            # Omitting an already-absent legacy syscall is a no-op, not a
            # meaningful mutation. Test that mutant in the legacy-present mode.
            if name == 'omit-epoll-wait' and not legacy:
                continue
            src = Path(tmp)/'control.c'
            exe = Path(tmp)/'control'
            prefix = '#include <stdbool.h>\n#include <stdint.h>\n#include <stdio.h>\n'
            prefix += '#include "'+str(root/'kernel/arch/x86_64/include/arch/unistd.h')+'"\n'
            if not legacy:
                prefix += '#undef __NR_epoll_wait\n'
            src.write_text(prefix+body+harness)
            subprocess.run([os.environ.get('CC','cc'), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
            r = subprocess.run([str(exe)], text=True, capture_output=True, timeout=2)
            label=f'{name} legacy_present={int(legacy)}'
            if name == 'normal':
                print(label+'\n'+r.stdout, end='', flush=True)
                if r.returncode:
                    raise SystemExit(r.returncode)
            else:
                mismatches=[line for line in r.stdout.splitlines() if line.startswith('FAIL ')]
                if r.returncode != 1 or not mismatches:
                    raise SystemExit(f'{label}: expected checked mismatch; got {r.returncode}\n{r.stdout}{r.stderr}')
                print(f'rejected {label}: {len(mismatches)} mismatches; first: {mismatches[0]}', flush=True)
# Inspect macro availability only: do not compile or execute non-x86 targets.
for arch in ['x86_64', 'aarch64', 'riscv64']:
    header=(root/f'kernel/arch/{arch}/include/arch/unistd.h').read_text()
    for required in ['__NR_epoll_pwait', '__NR_epoll_pwait2']:
        if not any(line.startswith('#define '+required+' ') for line in header.splitlines()):
            raise SystemExit(f'{arch}: missing portable syscall macro {required}')
print('required syscall macro presence inspected in all three headers; NOT cross-build proof', flush=True)
