#!/usr/bin/env python3
"""Type physical input with explicit QMP transitions (preferred) or legacy HMP.

Usage: qemu-type.py [--qmp] SOCK TEXT...; literal \n becomes Return.
HMP sendkey uses a VIRTUAL-time release timer: host sleeps cannot guarantee
non-overlap under slow TCG (ARM evidence: tty became ty even at20ms/120ms).
QMP input-send-event emits explicit down/up events through the same emulated
keyboard devices. No guest command injection, clipboard, retries or test waiver.
"""
import argparse
import json
import socket
import sys
import time

M = {' ': 'spc', '\n': 'ret', '-': 'minus', '=': 'equal', '/': 'slash', '.': 'dot', ',': 'comma', ';': 'semicolon',
     "'": 'apostrophe', '|': 'shift-backslash', '\\': 'backslash', '>': 'shift-dot', '<': 'shift-comma', '_': 'shift-minus',
     '$': 'shift-4', '&': 'shift-7', '*': 'shift-8', '"': 'shift-apostrophe', ':': 'shift-semicolon', '~': 'shift-grave_accent',
     '!': 'shift-1', '(': 'shift-9', ')': 'shift-0', '[': 'bracket_left', ']': 'bracket_right', '+': 'shift-equal'}
HOLD_MS = 20
INTERVAL_SECONDS = .12


def prompt(sock):
    data = b''
    while not data.endswith(b'(qemu) '):
        part = sock.recv(65536)
        if not part:
            raise RuntimeError('QEMU monitor closed before prompt')
        data += part
        if len(data) > 1 << 20:
            raise RuntimeError('Unbounded QEMU monitor response')
    return data


class QMP:
    def __init__(self, sock):
        self.sock = sock
        self.stream = sock.makefile('rb')
        self.sequence = 0
        if 'QMP' not in self.read():
            raise RuntimeError('Missing QMP greeting')
        self.execute('qmp_capabilities')

    def read(self):
        line = self.stream.readline((1 << 20) + 1)
        if not line or len(line) > 1 << 20:
            raise RuntimeError('Closed or unbounded QMP response')
        return json.loads(line)

    def execute(self, command, arguments=None):
        self.sequence += 1
        message = dict(execute=command, id=self.sequence)
        if arguments is not None:
            message['arguments'] = arguments
        self.sock.sendall(json.dumps(message).encode() + b'\n')
        for _ in range(100):
            reply = self.read()
            if 'event' in reply:
                continue
            if reply.get('id') != self.sequence or 'error' in reply or 'return' not in reply:
                raise RuntimeError('QMP command failed: ' + str(reply))
            return
        raise RuntimeError('Too many asynchronous QMP events')

    def keys(self, keys, down):
        self.execute('input-send-event', {'events': [
            {'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': k}}}
            for k in keys]})


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--qmp', action='store_true')
    ap.add_argument('socket')
    ap.add_argument('text', nargs='+')
    args = ap.parse_args()
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(10)
        s.connect(args.socket)
        qmp = QMP(s) if args.qmp else None
        if qmp is None:
            prompt(s)
        for text in args.text:
            for ch in text.encode().decode('unicode_escape'):
                k = M.get(ch) or (('shift-' + ch.lower()) if ch.isupper() else ch)
                if not (ch.isascii() and (ch.isalnum() or ch in M)):
                    raise ValueError(f'Unsupported physical key: {ch!r}')
                if qmp is not None:
                    keys = k.split('-')
                    qmp.keys(keys, True)
                    try:
                        time.sleep(HOLD_MS / 1000)
                    finally:
                        qmp.keys(list(reversed(keys)), False)
                    time.sleep(INTERVAL_SECONDS - HOLD_MS / 1000)
                else:
                    s.sendall(f'sendkey {k} {HOLD_MS}\n'.encode())
                    response = prompt(s)
                    if b'Error' in response or b'unknown' in response.lower() or b'invalid' in response.lower():
                        raise RuntimeError(f'QEMU rejected physical key {k}')
                    time.sleep(INTERVAL_SECONDS)


if __name__ == '__main__':
    main()
