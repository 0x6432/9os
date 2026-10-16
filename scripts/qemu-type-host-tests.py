#!/usr/bin/env python3
"""Fake HMP protocol/timing controls, not proof of delivered guest input."""
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading
import time

p = pathlib.Path(os.environ.get('QEMU_TYPE_SOURCE', pathlib.Path(__file__).with_name('qemu-type.py')))

with tempfile.TemporaryDirectory() as d:
    for error in (False, True):
        path = str(pathlib.Path(d) / ('error' if error else 'good'))
        commands = []
        with socket.socket(socket.AF_UNIX) as server:
            server.bind(path); server.listen(1); server.settimeout(10)
            def serve():
                with server.accept()[0] as client:
                    client.sendall(b'QEMU fake host control\n(qemu) ')
                    f = client.makefile('rb')
                    while line := f.readline():
                        commands.append((line.decode().strip(), time.monotonic()))
                        client.sendall((b'Error: invalid key\n' if error else b'\n') + b'(qemu) ')
            t = threading.Thread(target=serve);t.start()
            r = subprocess.run([sys.executable, str(p), path, 'tty\\n'], capture_output=True, timeout=10)
            t.join(10);assert not t.is_alive()
        if error:
            assert r.returncode != 0 and len(commands) == 1
        else:
            assert r.returncode == 0, r.stderr
            assert [c for c, _ in commands] == ['sendkey t 20', 'sendkey t 20', 'sendkey y 20', 'sendkey ret 20']
            assert all(b[1] - a[1] >= .020 for a, b in zip(commands, commands[1:]))
print('PASS: legacy HMP request spacing and rejection controls (not proof of virtual key release)')
