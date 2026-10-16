#!/usr/bin/env python3
"""Refuse physical keys without full-line terminal and readiness witnesses.

The serial command status alone is insufficient: diagnostic ps output can contain
an unanchored grep pattern. This gate does not set focus or replace the later
physical PTY/UID/file proof. No keyboard retry or guest command injection.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys


def ready(text, step=3):
    lines = text.replace('\r', '').splitlines()
    codes = re.findall(r'^__RC' + str(step) + r'=(\d+)$', '\n'.join(lines), re.M)
    if codes != ['0']:
        return False
    for marker in ('KONSOLE_FOCUSED_TERMINAL_READY', 'ALPINE_PLASMA_INPUT_READY'):
        if lines.count(marker) != 1:
            return False
    terminal = lines.index('KONSOLE_FOCUSED_TERMINAL_READY')
    input_ready = lines.index('ALPINE_PLASMA_INPUT_READY')
    status = lines.index('__RC' + str(step) + '=0')
    if not terminal < input_ready < status:
        return False
    return not any(line.startswith(('KONSOLE_READINESS', 'alpine-plasma: FAIL', 'alpine-dolphin: FAIL',
                                   'ALPINE_PLASMA_FAILURE_PNG_BEGIN')) for line in lines)


def guarded_type(text, sender, step=3):
    if not ready(text, step):
        raise ValueError('Refusing physical keys: missing genuine full-line readiness witnesses')
    return sender()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--log', required=True)
    ap.add_argument('--ready-step', type=int, default=3)
    ap.add_argument('--qmp', required=True)
    ap.add_argument('text')
    a = ap.parse_args()
    try:
        return guarded_type(Path(a.log).read_text(errors='replace'), lambda: subprocess.run(
            [sys.executable, str(Path(__file__).with_name('qemu-type.py')), '--qmp', a.qmp, a.text]
        ).returncode, a.ready_step)
    except (OSError, ValueError) as e:
        print(str(e), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
