#!/usr/bin/env python3
"""Boot 9os in QEMU and run test commands on the serial console, expect-style.

usage: scripts/qemu-test.py ARCH [--smp N] [--boot-timeout S] [--timeout S] [--log FILE] CMD...
Each CMD runs as `CMD; echo __RC=$?` once the shell prompt is up; a CMD starting with "host:" runs on
the host instead (shell, must exit 0). With --net-test a host test server (scripts/net-host-server.py)
listens on @HP@ (the guest reaches it as 10.0.2.2:@HP@) and host port @FP@ forwards to guest TCP 8080 /
UDP 8081; both placeholders are substituted in every CMD. A test fails if its exit status
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
    ap.add_argument('--userland', choices=['source', 'alpine', 'alpine-desktop'], default='source')
    ap.add_argument('--memory', default=None, help='QEMU memory, e.g. 2048M for Alpine Qt')
    ap.add_argument('--boot-timeout', type=float, default=180)
    ap.add_argument('--timeout', type=float, default=240, help='per command')
    ap.add_argument('--log', default=None)
    ap.add_argument('--disk', action='append', default=[], help='raw image attached as virtio-blk (repeatable)')
    ap.add_argument('--cmdline', default=None, help='extra kernel command line')
    ap.add_argument('--net-test', action='store_true', help='host test server + port forwarding')
    ap.add_argument('--frozen-manifest', help='experimental x86 non-building fixture manifest')
    ap.add_argument('--monitor-socket', help='fresh monitor socket for frozen mode')
    ap.add_argument('--qmp-socket', help='fresh QMP socket for frozen mode')
    a = ap.parse_intermixed_args()
    def report_blocked(label, reason, target=None):
        # These are explicit non-results, never guest passes. A failure before
        # log creation remains visible on stdout/stderr instead.
        lines = ['FAIL   ' + label + ': ' + str(reason)] + ['SKIP   ' + cmd for cmd in a.cmds]
        text = '\n'.join(lines) + '\n'
        print(text, end='', flush=True)
        if target is not None:
            target.write(text.encode('utf-8', errors='replace')); target.flush()

    def refuse_frozen(reason):
        report_blocked('frozen preflight', reason)
        ap.error(reason)
    if a.frozen_manifest and (a.arch != 'x86_64' or a.userland != 'alpine' or a.smp != '4'
            or a.memory != '3072M' or a.sched or a.cmdline != 'root=/dev/vda1'
            or a.boot_timeout != 300 or a.timeout != 780
            or a.net_test or len(a.disk) != 1 or not a.log or os.environ.get('QEMU_EXTRA')):
        refuse_frozen('frozen mode requires the fixed x86 KMS configuration')
    import socket as _s
    def free_port():
        k = _s.socket(); k.bind(('127.0.0.1', 0)); n = k.getsockname()[1]; k.close(); return n
    server = None
    net = '-netdev user,id=n0 -device virtio-net-pci,netdev=n0'
    if a.net_test:
        hp, fp = free_port(), free_port()
        server = subprocess.Popen([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'net-host-server.py'), str(hp)],
                                  start_new_session=True)
        net = f'-netdev user,id=n0,hostfwd=tcp:127.0.0.1:{fp}-:8080,hostfwd=udp:127.0.0.1:{fp}-:8081 -device virtio-net-pci,netdev=n0'
        a.cmds = [c.replace('@HP@', str(hp)).replace('@FP@', str(fp)) for c in a.cmds]
    frozen_state = None
    log = None
    make = ['make', '-s', 'ARCH=' + a.arch, 'SMP=' + a.smp, 'run', 'QEMUFLAGS=-display none ' + os.environ.get('QEMU_EXTRA', '') +
            ''.join(f' -drive file={d},if=none,id=vd{k},format=raw -device virtio-blk-pci,drive=vd{k},serial=9os-disk{k}' for k, d in enumerate(a.disk))]
    make.insert(3, 'QEMU_NET=' + net)
    make.insert(3, 'USERLAND=' + a.userland)
    if a.memory: make.insert(3, 'QEMU_MEMORY=' + a.memory)
    if a.sched: make.insert(3, 'SCHED=' + a.sched)
    if a.cmdline: make.insert(3, 'CMDLINE=' + a.cmdline)
    if a.frozen_manifest:
        # Frozen mode is intentionally narrow: no implicit make, arbitrary
        # QEMU_EXTRA, network forwarder, config changes or second writable disk.
        try:
            import importlib.util
            spec = importlib.util.spec_from_file_location('qemu_frozen', os.path.join(os.path.dirname(__file__), 'qemu-frozen.py'))
            frozen = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(frozen)
            make, frozen_state = frozen.load(a.frozen_manifest, a.disk[0], a.monitor_socket, a.qmp_socket)
        except (OSError, ValueError) as e:
            refuse_frozen('frozen preflight failed: ' + str(e))
    elif a.monitor_socket or a.qmp_socket:
        refuse_frozen('frozen sockets require --frozen-manifest')
    # In frozen mode, never start a guest unless a fresh host log can be
    # created without following or truncating an existing fixture or log.
    if frozen_state is not None:
        try:
            fd = os.open(a.log, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            log = os.fdopen(fd, 'wb')
        except OSError as e:
            refuse_frozen('cannot create fresh frozen log: ' + str(e))
    else:
        log = open(a.log, 'wb') if a.log else None
    try:
        p = subprocess.Popen(make, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             start_new_session=True)
    except OSError as e:
        if frozen_state is None:
            raise
        report_blocked('launcher startup', e, log)
        try:
            frozen.verify_after(frozen_state)
        except (OSError, ValueError) as check_error:
            message = 'FAIL   frozen fixture verification: ' + str(check_error) + '\n'
            print(message, end='', flush=True)
            if log: log.write(message.encode('utf-8', errors='replace')); log.flush()
        if log: log.close()
        raise SystemExit(1) from None
    try:
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
            # small chunks: a 16550 FIFO holds 16 bytes and the guest may be slow to drain it
            b = s.encode()
            for i in range(0, len(b), 8):
                p.stdin.write(b[i:i + 8]); p.stdin.flush()
                time.sleep(0.03)

        results = []
        unexpected_exit = False
        attempted_cmds = 0
        # the boot console shows a prompt once the shell is up; poke it with newlines until we see one
        boot_deadline = time.time() + a.boot_timeout
        pump(min(boot_deadline, time.time() + 20), lambda: b'Welcome to 9os' in buf)
        pump(min(boot_deadline, time.time() + 5), lambda: buf.rstrip().endswith(b'login:'))
        ok = False
        # M31: the console runs getty+login; log in as root (no password) when asked
        at_login = lambda: buf.rstrip().endswith(b'login:')
        while not ok and time.time() < boot_deadline and not state['panic'] and p.poll() is None:
            try:
                if at_login(): send('root\n')
                else: send('\n')
            except BrokenPipeError:
                break
            ok = pump(min(boot_deadline, time.time() + 10), lambda: buf.rstrip().endswith(b'#') or at_login())
            if ok and at_login(): ok = False
        if ok:
            pump(time.time() + 2, lambda: False)       # let the rest of the banner settle
        if not ok:
            print('\n*** boot failed (no shell prompt)%s' % (' - kernel panic' if state['panic'] else ''))
            results.append(('boot', False))
        else:
            results.append(('boot', True))
            root_ok = True
            if frozen_state is not None:
                # A root= cmdline is not proof of a successful mount: the kernel
                # can fall back to initramfs. Stop before any requested step.
                start = len(buf)
                try:
                    send(frozen.root_gate_command() + '; echo __FROZENROOT=$?\n')
                    rx = re.compile(rb'(?m)^__FROZENROOT=(\d+)\n')
                    pump(time.time() + a.timeout, lambda: rx.search(bytes(buf[start:])) is not None)
                    m = rx.search(bytes(buf[start:]))
                    root_ok = bool(m) and m.group(1) == b'0' and not state['panic'] and p.poll() is None
                except BrokenPipeError:
                    root_ok = False
                results.append(('frozen root preflight', root_ok))
            frozen_failed = frozen_state is not None and not root_ok
            for i, cmd in enumerate(a.cmds if root_ok else []):
                # Never execute later guest or host steps after the VM dies.
                if state['panic'] or p.poll() is not None: break
                if cmd.startswith('host:'):
                    if frozen_state is not None:
                        out, passed = frozen.run_host(cmd[5:], a.timeout)
                    else:
                        try:
                            hr = subprocess.run(cmd[5:], shell=True, timeout=a.timeout, capture_output=True)
                            out = hr.stdout + hr.stderr
                            passed = hr.returncode == 0
                        except subprocess.TimeoutExpired:
                            out, passed = b'(timeout)', False
                    sys.stdout.buffer.write(b'\n[host] ' + cmd[5:].encode() + b'\n' + out); sys.stdout.flush()
                    if log: log.write(b'\n[host] ' + cmd[5:].encode() + b'\n' + out); log.flush()
                    results.append((cmd, passed))
                    attempted_cmds += 1
                    # Frozen diagnostics fail closed after any failed host
                    # helper; all modes stop if the pre-type screenshot fails.
                    if not passed and (frozen_state is not None or
                            cmd.startswith('host:python3 scripts/qemu-screenshot.py ')):
                        frozen_failed = frozen_state is not None
                        break
                    continue
                mark = '__RC%d=' % i
                # wait for the prompt so the shell is reading (line editing in raw mode) before typing;
                # input typed while the previous command was still finishing could get lost
                pump(time.time() + 5, lambda: buf.rstrip().endswith(b'#'))
                if state['panic'] or p.poll() is not None: break
                start = len(buf)
                try:
                    # Preserve the command status while forcing the marker onto
                    # its own line, even if command output lacks a newline.
                    send('%s; __qemu_test_rc=$?; echo; echo %s$__qemu_test_rc\n' % (cmd, mark))
                except BrokenPipeError:
                    print('\n*** guest command aborted: serial pipe closed')
                    results.append((cmd, False))
                    attempted_cmds += 1
                    unexpected_exit = True
                    frozen_failed = frozen_state is not None
                    break
                # A prefixed marker is output, not command completion. A guest
                # can still forge a complete marker line; this is not authentication.
                rx = re.compile(rb'(?m)^' + re.escape(mark.encode()) + rb'(\d+)\n')
                got = pump(time.time() + a.timeout, lambda: rx.search(bytes(buf[start:])) is not None)
                out = bytes(buf[start:])
                m = rx.search(out)
                # skip the echoed command line itself (it contains the marker text but no digits+newline)
                # Ignore only a confirmed echo; otherwise inspect the first output line.
                body = out[:m.start()] if m else out
                if m and b'\n' in body:
                    first, rest = body.split(b'\n', 1)
                    echo = ('%s; __qemu_test_rc=$?; echo; echo %s$__qemu_test_rc' % (cmd, mark)).encode()
                    if first == echo:
                        body = rest
                passed = bool(m) and m.group(1) == b'0' and not re.search(rb'\bFAIL', body) and not state['panic']
                results.append((cmd, passed))
                attempted_cmds += 1
                # Do not send another command while a timed-out or otherwise
                # failed frozen guest command may still be active.
                if frozen_state is not None and not passed:
                    frozen_failed = True
                    break
                if state['panic'] or p.poll() is not None: break
            if p.poll() is not None:
                unexpected_exit = True
            elif frozen_failed or (frozen_state is not None and state['panic']):
                print('FAIL   frozen failure: terminating without further guest commands')
            else:
                try:
                    send('poweroff\n')
                    pump(time.time() + 20, lambda: False)
                except BrokenPipeError:
                    unexpected_exit = True
        try: os.killpg(p.pid, signal.SIGKILL)
        except ProcessLookupError: pass
        if server:
            try: os.killpg(server.pid, signal.SIGKILL)
            except ProcessLookupError: pass
        frozen_error = None
        if frozen_state is not None:
            try:
                p.wait(timeout=10)
                frozen.verify_after(frozen_state)
            except (OSError, ValueError, subprocess.TimeoutExpired) as e:
                frozen_error = str(e)
        # Every unattempted step is explicitly skipped and makes the run incomplete.
        results.extend((cmd, None) for cmd in a.cmds[attempted_cmds:])
        print('\n==== 9os test summary (%s, SMP=%s%s) ====' % (a.arch, a.smp, ', SCHED=' + a.sched if a.sched else ''))
        for name, passed in results:
            print('%-6s %s' % ('SKIP' if passed is None else 'PASS' if passed else 'FAIL', name))
        if state['panic']: print('FAIL   kernel panic detected')
        if unexpected_exit: print('FAIL   VM exited before requested shutdown')
        if frozen_error: print('FAIL   frozen fixture verification: ' + frozen_error)
        bad = [n for n, ok_ in results if ok_ is not True] or (['panic'] if state['panic'] else []) or (['unexpected VM exit'] if unexpected_exit else [])
        if frozen_error: bad.append('frozen fixture verification')
        print('%d/%d passed' % (sum(1 for _, x in results if x), len(results)))
        sys.exit(1 if bad else 0)
    finally:
        # Frozen launches must not leave a QEMU process group behind on a
        # Python error or interruption. Preserve the original failure while
        # reporting cleanup/identity errors as additional failures.
        if frozen_state is not None:
            active_error = sys.exc_info()[1]
            cleanup_errors = []
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            except OSError as e:
                cleanup_errors.append('process group termination: ' + str(e))
            try:
                p.wait(timeout=10)
            except (OSError, subprocess.TimeoutExpired) as e:
                cleanup_errors.append('process reap: ' + str(e))
            try:
                frozen.verify_after(frozen_state)
            except (OSError, ValueError) as e:
                cleanup_errors.append('fixture verification: ' + str(e))
            if log:
                try: log.close()
                except OSError as e: cleanup_errors.append('log close: ' + str(e))
            for error in cleanup_errors:
                print('FAIL   frozen cleanup: ' + error, file=sys.stderr)
            if cleanup_errors and (active_error is None or
                    (isinstance(active_error, SystemExit) and active_error.code in (None, 0))):
                raise SystemExit(1)

main()
