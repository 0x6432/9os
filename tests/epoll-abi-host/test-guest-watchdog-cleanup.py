#!/usr/bin/env python3
"""Native x86-only R1 regression control: parent STOP + signal-blocked child.

The real consumer is never modified. The same fixed run_isolated(timeout=6)
used by guest-watchdog-control.py must raise TimeoutExpired, not accept/rescue
this stalled supervisor. The descendant must be killed AND reaped before that
failure is observed. An unrelated direct child must remain alive/unreaped.
"""
from pathlib import Path
import ctypes
import hashlib
import os
import platform
import signal
import subprocess
import tempfile
import time
from isolated_process import run_isolated

if platform.machine() != 'x86_64':
    raise SystemExit('native x86_64 cleanup control only; no non-x86 authorization')
root = Path(__file__).resolve().parents[2]
source = (root/'userland/tests/epollabitest.c').read_text()
anchor = '    if(child==0) _exit(run_tests());'
assert source.count(anchor) == 1
mutated = source.replace(anchor, '''    if(child==0) {
        sigset_t all;
        if(sigfillset(&all) || sigprocmask(SIG_SETMASK,&all,NULL)) _exit(2);
        printf("control-child=%ld\\n",(long)getpid());
        for (;;) pause();
    }
    printf("control-supervisor=%ld\\n",(long)getpid());
    raise(SIGSTOP); /* Deliberately stall before the real five-second loop. */''')
print('host:', platform.platform(), flush=True)
print('production-consumer sha256:', hashlib.sha256(source.encode()).hexdigest(), flush=True)
print('temporary supervisor-stall control sha256:', hashlib.sha256(mutated.encode()).hexdigest(), flush=True)
print('mutation: block/hang child; STOP supervisor before its original loop', flush=True)
with tempfile.TemporaryDirectory(prefix='9os-epoll-cleanup-') as tmp:
    src = Path(tmp)/'supervisor-stall.c'
    exe = Path(tmp)/'supervisor-stall'
    src.write_text(mutated)
    cmd = [os.environ.get('CC','cc'),'-std=gnu11','-O2','-Wall','-Wextra','-Werror',
           '-Wno-unused-function','-D_FORTIFY_SOURCE=2',str(src),'-o',str(exe)]
    print('compile:', ' '.join(cmd).replace(tmp,'<tmp>'), flush=True)
    subprocess.run(cmd, check=True)
    # A real unrelated child, not in the disposable control session/group.
    unrelated = subprocess.Popen(['sleep', '20'])
    libc = ctypes.CDLL(None, use_errno=True)
    before = ctypes.c_int()
    if libc.prctl(37, ctypes.byref(before), 0, 0, 0):
        unrelated.kill(); unrelated.wait()
        raise OSError(ctypes.get_errno(), 'PR_GET_CHILD_SUBREAPER before control')
    start = time.monotonic()
    try:
        try:
            run_isolated([str(exe)], timeout=6)
        except subprocess.TimeoutExpired as failure:
            elapsed = time.monotonic()-start
            print(f'EXPECTED outer failure: {type(failure).__name__}, timeout={failure.timeout}, observed={elapsed:.6f}s', flush=True)
            assert failure.timeout == 6 and elapsed >= 6
            assert failure.cleanup_returncode == -signal.SIGKILL
            lines = failure.cleanup_stdout.splitlines()
            child = int(next(x.split('=',1)[1] for x in lines if x.startswith('control-child=')))
            parent = int(next(x.split('=',1)[1] for x in lines if x.startswith('control-supervisor=')))
            assert parent == failure.cleanup_pgid
            assert (child, signal.SIGKILL) in failure.cleanup_reaped, failure.cleanup_reaped
            for pid in (parent, child):
                try: os.kill(pid,0)
                except ProcessLookupError: pass
                else: raise AssertionError(f'control process still exists: {pid}')
            try: os.killpg(parent,0)
            except ProcessLookupError: pass
            else: raise AssertionError('isolated control group still exists')
            try: os.waitpid(child,os.WNOHANG)
            except ChildProcessError: pass
            else: raise AssertionError('control child was not already reaped')
            assert unrelated.poll() is None
            after = ctypes.c_int()
            if libc.prctl(37,ctypes.byref(after),0,0,0):
                raise OSError(ctypes.get_errno(),'PR_GET_CHILD_SUBREAPER after control')
            assert after.value == before.value
            print('PASS: original six-second timeout propagated after supervisor kill and adopted child SIGKILL/reap',flush=True)
            print('PASS: control group absent; child absent/already reaped; unrelated child untouched; subreaper state restored',flush=True)
        else:
            raise AssertionError('stalled supervisor was incorrectly accepted/rescued')
    finally:
        unrelated.kill()
        unrelated.wait()
print('Native Linux cleanup control only; not guest/kernel validation.',flush=True)
