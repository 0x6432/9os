#!/usr/bin/env python3
"""Native x86-only negative control for the standalone five-second supervisor.

This does NOT run a guest, alter the runtime consumer, or rerun historical tests.
The temporary child blocks signals and hangs instead of running the ABI cases.
Only the real, unmodified parent watchdog must terminate/reap it and return 124.
"""
from pathlib import Path
import hashlib
import os
import platform
import subprocess
import tempfile
import time
from isolated_process import run_isolated

if platform.machine() != 'x86_64':
    raise SystemExit('native x86_64 control only; no non-x86 test authorized')
root=Path(__file__).resolve().parents[2]
source=(root/'userland/tests/epollabitest.c').read_text()
anchor='    if(child==0) _exit(run_tests());'
assert source.count(anchor)==1
mutated=source.replace(anchor,'''    if(child==0) {
        sigset_t all;
        if(sigfillset(&all) || sigprocmask(SIG_SETMASK,&all,NULL)) _exit(2);
        for (;;) pause();
    }''')
print('host:',platform.platform(),flush=True)
print('source sha256:',hashlib.sha256(source.encode()).hexdigest(),flush=True)
print('temporary negative-control sha256:',hashlib.sha256(mutated.encode()).hexdigest(),flush=True)
print('mutation: replace child run_tests with block-all-signals + permanent pause',flush=True)
with tempfile.TemporaryDirectory(prefix='9os-epollabitest-watchdog-') as tmp:
    src=Path(tmp)/'watchdog-control.c'
    exe=Path(tmp)/'watchdog-control'
    src.write_text(mutated)
    cmd=[os.environ.get('CC','cc'),'-std=gnu11','-O2','-Wall','-Wextra','-Werror',
         '-Wno-unused-function','-D_FORTIFY_SOURCE=2',str(src),'-o',str(exe)]
    print('compile:', ' '.join(cmd).replace(tmp,'<tmp>'),flush=True)
    subprocess.run(cmd,check=True)
    start=time.monotonic()
    result=run_isolated([str(exe)],timeout=6)
    elapsed=time.monotonic()-start
    print(result.stdout,end='',flush=True)
    print(result.stderr,end='',flush=True)
    print(f'exit={result.returncode}; observed elapsed={elapsed:.6f}s',flush=True)
    if result.returncode!=124 or 'FAIL five-second process watchdog' not in result.stderr or elapsed<5:
        raise SystemExit('FAIL: five-second watchdog negative control was not rejected accurately')
print('PASS: original five-second supervisor rejected signal-blocked child hang',flush=True)
print('Six-second outer timeout remains a failure; isolated group kill/reap runs before re-raising exceptions.',flush=True)
print('Native Linux control only; NOT 9os guest runtime validation.',flush=True)
