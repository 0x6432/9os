#!/usr/bin/env python3
"""Boot 9os in QEMU and run test commands on the serial console, expect-style.

usage: scripts/qemu-test.py ARCH [--smp N] [--boot-timeout S] [--timeout S] [--log FILE] CMD...
Each CMD runs as `CMD; echo __RC=$?` once the shell prompt is up. A test fails if its exit status
is non-zero or its output contains FAIL; the run fails on any kernel panic/oops.
Exit status: 0 if every test passed."""
import argparse, os, re, select, signal, subprocess, sys, time

ANSI = re.compile(rb'\x1b\[[0-9;?]*[a-zA-Z]|\r')
PROMPT = b'# '
PANIC = re.compile(rb'(?i)kernel panic|\bpanic:|page fault in kernel|\boops\b|unhandled exception|double fault')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('arch')
    ap.add_argument('cmds', nargs='*')
    ap.add_argument('--smp', default='4')
    ap.add_argument('--sched', default='')
    ap.add_argument('--boot-timeout', type=float, default=180)
    ap.add_argument('--timeout', type=float, default=240, help='per command')
    ap.add_argument('--log', default=None)
    a = ap.parse_args()
    make = ['make', '-s', 'ARCH=' + a.arch, 'SMP=' + a.smp, 'run', 'QEMUFLAGS=-display none']
    if a.sched: make.insert(3, 'SCHED=' + a.sched)
    p = subprocess.Popen(make, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         start_new_session=True)
    log = open(a.log, 'wb') if a.log else None
    buf = bytearray()
    state = {'panic': False}

    def pump(deadline, pred):
        while time.time() < deadline:
            r, _, _ = select.select([p.stdout], [], [], 0.5)
            if r:
                d = os.read(p.stdout.fileno(), 65536)
                if not d: return False
                d = ANSI.sub(b'', d)
                buf.extend(d)
                if log: log.write(d); log.flush()
                sys.stdout.buffer.write(d); sys.stdout.flush()
                if PANIC.search(bytes(buf[-4096:])): state['panic'] = True; return False
                if pred(): return True
            elif p.poll() is not None:
                return False
        return False

    def send(s):
        p.stdin.write(s.encode()); p.stdin.flush()

    results = []
    # the boot console shows a prompt once the shell is up; poke it with newlines until we see one
    boot_deadline = time.time() + a.boot_timeout
    ok = pump(min(boot_deadline, time.time() + 20), lambda: b'Welcome to 9os' in buf)
    while not ok and time.time() < boot_deadline and not state['panic'] and p.poll() is None:
        send('\n')
        ok = pump(min(boot_deadline, time.time() + 10), lambda: buf.rstrip().endswith(b'#'))
    if ok:
        pump(time.time() + 2, lambda: False)       # let the rest of the banner settle
    if not ok:
        print('\n*** boot failed (no shell prompt)%s' % (' - kernel panic' if state['panic'] else ''))
        results.append(('boot', False))
    else:
        results.append(('boot', True))
        for i, cmd in enumerate(a.cmds):
            mark = '__RC%d=' % i
            start = len(buf)
            send('%s; echo %s$?\n' % (cmd, mark))
            rx = re.compile(re.escape(mark.encode()) + rb'(\d+)\n')
            got = pump(time.time() + a.timeout, lambda: rx.search(bytes(buf[start:])) is not None)
            out = bytes(buf[start:])
            m = rx.search(out)
            # skip the echoed command line itself (it contains the marker text but no digits+newline)
            body = out[out.find(b'\n') + 1:m.start()] if m else out
            passed = bool(m) and m.group(1) == b'0' and not re.search(rb'\bFAIL', body) and not state['panic']
            results.append((cmd, passed))
            if state['panic'] or p.poll() is not None: break
        send('poweroff\n')
        pump(time.time() + 20, lambda: False)
    try: os.killpg(p.pid, signal.SIGKILL)
    except ProcessLookupError: pass
    print('\n==== 9os test summary (%s, SMP=%s%s) ====' % (a.arch, a.smp, ', SCHED=' + a.sched if a.sched else ''))
    for name, passed in results: print('%-6s %s' % ('PASS' if passed else 'FAIL', name))
    if state['panic']: print('FAIL   kernel panic detected')
    bad = [n for n, ok_ in results if not ok_] or (['panic'] if state['panic'] else [])
    print('%d/%d passed' % (sum(1 for _, x in results if x), len(results)))
    sys.exit(1 if bad else 0)

main()
