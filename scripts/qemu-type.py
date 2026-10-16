#!/usr/bin/env python3
"""Type physical guest input via HMP, with non-overlapping key presses.

Usage: qemu-type.py SOCK TEXT...; literal \\n becomes Return.
QEMU sendkey defaults to a 100ms hold. Sending every 80ms can coalesce repeated
keys (actual ARM CI screenshot: `tty` became `ty`). Explicitly hold for 20ms,
then leave 100ms release margin. No command retries or guest-side injection.
"""
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


def main():
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(10)
        s.connect(sys.argv[1])
        prompt(s)
        for text in sys.argv[2:]:
            for ch in text.encode().decode('unicode_escape'):
                k = M.get(ch) or (('shift-' + ch.lower()) if ch.isupper() else ch)
                if not (ch.isascii() and (ch.isalnum() or ch in M)):
                    raise ValueError(f'Unsupported physical key: {ch!r}')
                s.sendall(f'sendkey {k} {HOLD_MS}\n'.encode())
                response = prompt(s)
                if b'Error' in response or b'unknown' in response.lower() or b'invalid' in response.lower():
                    raise RuntimeError(f'QEMU rejected physical key {k}')
                time.sleep(INTERVAL_SECONDS)


if __name__ == '__main__':
    main()
