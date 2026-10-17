#!/usr/bin/env python3
"""Real private-bus IPC against an explicit synthetic server, NOT GUI proof.

Requires native gio-2.0 development headers and an already installed signed
x86_64 Alpine root. Builds only tiny C consumers; never Qt/Mesa/LLVM libraries.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import select
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
TOP = HERE.parents[1]


def ready(process, prefix):
    if not select.select([process.stdout], [], [], 5)[0]:
        raise RuntimeError('private IPC fixture startup timeout')
    line = process.stdout.readline().strip()
    assert line.startswith(prefix), (line, process.poll())
    return line


def stop(process):
    if process.poll() is None:
        process.terminate()
    try:
        process.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate()
        raise


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--signed-root', type=Path, default=TOP/'userland/alpine-root-x86_64')
    ap.add_argument('--consumer', choices=['native', 'signed-musl'], default='native')
    ap.add_argument('--log-dir', type=Path)
    args = ap.parse_args()
    assert platform.machine() == 'x86_64', 'native protocol controls are x86_64 only'
    signed = args.signed_root.resolve()
    assert json.loads((signed/'etc/9os-alpine.json').read_text())['arch'] == 'x86_64'
    if args.log_dir:
        args.log_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='9os-private-a11y-') as temp:
        work = Path(temp)
        strict = ['-O2', '-Wall', '-Wextra', '-Werror', '-D_FORTIFY_SOURCE=2']
        cc = os.environ.get('CC', 'cc')
        gio = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'gio-2.0'], text=True))
        subprocess.run([cc, *strict, str(HERE/'mock-server.c'), *gio, '-o', str(work/'server')], check=True)
        source = HERE/'terminal-dbus.c'
        binary = work/'consumer'
        if args.consumer == 'native':
            subprocess.run([cc, *strict, str(source), '-ldl', '-o', str(binary)], check=True)
            consumer = [str(binary)]
        else:
            subprocess.run(['clang', '--target=x86_64-linux-musl', '--sysroot='+str(signed), *strict,
                            '-fPIE', '-pie', '-fuse-ld=lld', '-nostdlib',
                            str(signed/'usr/lib/Scrt1.o'), str(signed/'usr/lib/crti.o'), str(source),
                            '-L'+str(signed/'usr/lib'), '-lc', str(signed/'usr/lib/crtn.o'),
                            '-Wl,--dynamic-linker,/lib/ld-musl-x86_64.so.1', '-o', str(binary)], check=True)
            consumer = [str(signed/'lib/ld-musl-x86_64.so.1'), '--library-path',
                        str(signed/'lib')+':'+str(signed/'usr/lib'), str(binary)]
        config = work/'bus.conf'
        config.write_text('''<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd"><busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth><policy context="default"><allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>''')
        daemon = subprocess.Popen([str(signed/'lib/ld-musl-x86_64.so.1'), '--library-path',
                                   str(signed/'lib')+':'+str(signed/'usr/lib'),
                                   str(signed/'usr/bin/dbus-daemon'), '--nofork', '--print-address=1',
                                   '--config-file='+str(config)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            address = ready(daemon, 'unix:')
            env = {**os.environ, 'DBUS_SESSION_BUS_ADDRESS': address}
            negatives = ['wrong-pid', 'unfocused', 'hidden', 'invisible', 'wrong-role',
                         'short-state', 'long-state', 'bad-address', 'empty', 'duplicate-app',
                         'many-apps', 'many-children', 'bad-path', 'foreign-child', 'cycle',
                         'menu-decoy', 'remote-error', 'blocked-role', 'address-signature',
                         'children-signature', 'role-signature', 'state-signature']
            for mode in ['valid', *negatives]:
                server = subprocess.Popen([str(work/'server'), mode], env=env, stdout=subprocess.PIPE,
                                          stderr=subprocess.PIPE, text=True)
                try:
                    ready(server, 'MOCK_SERVER_READY')
                    pid = server.pid + (mode == 'wrong-pid')
                    result = subprocess.run([*consumer, str(pid)], env=env, text=True,
                                            capture_output=True, timeout=10)
                    if args.log_dir:
                        (args.log_dir/(mode+'.out')).write_text(result.stdout+result.stderr)
                    assert result.returncode == (0 if mode == 'valid' else 1), (mode, result.returncode, result.stdout[-3000:], result.stderr[-1000:])
                    if mode == 'valid':
                        assert 'DBUS_A11Y_TERMINAL focused=1 showing=1 visible=1' in result.stdout
                        assert 'applications=1 models=1' in result.stdout
                        assert f'pid={pid} target={pid}' in result.stdout
                    if mode == 'menu-decoy':
                        assert 'DBUS_A11Y_TERMINAL ' not in result.stdout
                    if mode.endswith('-signature'):
                        assert 'DBUS_A11Y_ERROR reason=reply-signature' in result.stdout
                    print('PASS synthetic protocol', mode, flush=True)
                finally:
                    stop(server)
            for argv in [[], ['1', '2'], [''], ['0'], ['-1'], ['1x'], ['2147483648']]:
                result = subprocess.run([*consumer, *argv], env=env, capture_output=True, timeout=5)
                assert result.returncode == 2, argv
            print(f'23/23 real private IPC controls + 7 CLI bounds ({args.consumer}); synthetic only, not Qt or GUI proof')
        finally:
            stop(daemon)


if __name__ == '__main__':
    main()
