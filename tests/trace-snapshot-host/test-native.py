#!/usr/bin/env python3
"""Real ptrace on native x86 synthetic children; no desktop proof."""
import argparse
import os
from pathlib import Path
import platform
import re
import select
import signal
import subprocess
import tempfile

TOP = Path(__file__).resolve().parents[2]

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--consumer', choices=['native', 'signed-musl'], default='native')
    args = ap.parse_args()
    assert platform.machine() == 'x86_64', 'local x86 only'
    strict = ['-O2', '-Wall', '-Wextra', '-Werror', '-D_FORTIFY_SOURCE=2']
    with tempfile.TemporaryDirectory(prefix='9os-snapshot-') as tmp:
        d = Path(tmp)
        source = TOP/'userland/tests/trace-snapshot/trace-snapshot.c'
        cc = os.environ.get('CC', 'cc')
        subprocess.run([cc, *strict, str(Path(__file__).with_name('maps-control.c')),
                        '-o', str(d/'maps-control')], check=True)
        model = subprocess.run([str(d/'maps-control')], capture_output=True, text=True, timeout=5)
        assert model.returncode == 0, model.stdout+model.stderr
        print(model.stdout.splitlines()[-1], flush=True)
        subprocess.run([cc, *strict, str(Path(__file__).with_name('target.c')), '-o', str(d/'target')], check=True)
        if args.consumer == 'native':
            subprocess.run([cc, *strict, str(source), '-o', str(d/'snapshot')], check=True)
            consumer = [str(d/'snapshot')]
        else:
            root = TOP/'userland/alpine-root-x86_64'
            subprocess.run(['clang', '--target=x86_64-linux-musl', '--sysroot='+str(root), *strict,
                            '-fPIE', '-pie', '-fuse-ld=lld', '-nostdlib', str(root/'usr/lib/Scrt1.o'),
                            str(root/'usr/lib/crti.o'), str(source), '-L'+str(root/'usr/lib'), '-lc',
                            str(root/'usr/lib/crtn.o'), '-Wl,--dynamic-linker,/lib/ld-musl-x86_64.so.1',
                            '-o', str(d/'snapshot')], check=True)
            consumer = [str(root/'lib/ld-musl-x86_64.so.1'), str(d/'snapshot')]
        for argv in [[], [''], ['0'], ['-1'], ['1x'], [' 1'], ['+1'], ['2147483648'], ['1','2']]:
            r = subprocess.run([*consumer, *argv], capture_output=True, text=True, timeout=5)
            assert r.returncode == 2 and not r.stdout, (argv, r)
        print('PASS 9 CLI rejection controls', flush=True)
        r = subprocess.run([*consumer, '2147483647'], capture_output=True, text=True, timeout=5)
        assert r.returncode == 1 and 'phase=seize' in r.stdout and 'REG ' not in r.stdout, r
        print('PASS nonexistent target is not a snapshot', flush=True)
        for mode in ['busy', 'blocked', 'stopped', 'many-maps', 'too-many-maps']:
            child = subprocess.Popen([str(d/'target'), mode], stdout=subprocess.PIPE, text=True)
            try:
                assert select.select([child.stdout], [], [], 5)[0]
                assert child.stdout.readline().strip() == f'READY {child.pid}'
                if mode == 'stopped':
                    for _ in range(100):
                        status = Path(f'/proc/{child.pid}/status').read_text()
                        if re.search(r'^State:\s+T', status, re.M): break
                        import time
                        time.sleep(.01)
                    else: raise AssertionError('fixture did not stop')
                r = subprocess.run([*consumer, str(child.pid)], capture_output=True, text=True, timeout=5)
                assert 'TRACE_SNAPSHOT_DETACHED' in r.stdout, r.stdout+r.stderr
                assert child.poll() is None
                status = Path(f'/proc/{child.pid}/status').read_text()
                assert re.search(r'^TracerPid:\s+0$', status, re.M), status
                if mode == 'stopped':
                    assert r.returncode == 1 and 'phase=unexpected-stop' in r.stdout, r.stdout
                    assert re.search(r'^State:\s+T', status, re.M), status
                    assert 'TRACE_SNAPSHOT_REGS' not in r.stdout
                else:
                    assert r.returncode == (1 if mode == 'too-many-maps' else 0), r.stdout+r.stderr
                    assert 'TRACE_SNAPSHOT_DETACHED signal=0' in r.stdout
                    assert len(re.findall(r'^REG ', r.stdout, re.M)) == 27
                    assert len(re.findall(r'^STACK ', r.stdout, re.M)) == 32
                    m = re.search(r'TRACE_SNAPSHOT_REGS pc=([0-9a-f]+) sp=([0-9a-f]+) bytes=216', r.stdout)
                    assert m, r.stdout
                    pc, sp = [int(x,16) for x in m.groups()]
                    mappings = re.findall(r'^MAP ([0-9a-f]+)-([0-9a-f]+) (....)', r.stdout, re.M)
                    assert len(mappings) <= 2
                    if mode == 'too-many-maps':
                        assert re.search(r'MAPS_END chunks=4096 .* truncated=1 error=0 malformed=0', r.stdout), r.stdout
                        assert 'TRACE_SNAPSHOT_END result=1' in r.stdout
                    else:
                        assert any(int(a,16)<=pc<int(b,16) and 'x' in perm for a,b,perm in mappings)
                        assert any(int(a,16)<=sp<int(b,16) and 'r' in perm for a,b,perm in mappings)
                        scan = re.search(r'MAPS_END chunks=(\d+) selected=(\d+) pc=1 sp=1 truncated=0 error=0 malformed=0', r.stdout)
                        assert scan, r.stdout
                        if mode == 'many-maps':
                            assert int(scan[1]) > 256, r.stdout
                    assert not re.search(r'^State:\s+[Tt]', status, re.M), status
                print('PASS real synthetic tracee', mode, args.consumer, flush=True)
            finally:
                if child.poll() is None: child.kill()
                child.wait(timeout=5)
    print('15/15 native-kernel controls; synthetic only, not 9os or Konsole proof', flush=True)

if __name__ == '__main__': main()
