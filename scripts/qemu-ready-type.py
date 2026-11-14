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

MAX_DIAGNOSTIC_LOG_BYTES = 64 * 1024 * 1024

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


def input_capture_ready(text, capture_ready_step):
    if not isinstance(capture_ready_step, int) or capture_ready_step < 0:
        return False
    lines = text.replace('\r', '').splitlines()
    started = [i for i, line in enumerate(lines)
               if line == 'INPUT_CHAIN_CAPTURE_STARTED seconds=45']
    witnesses = [(i, line) for i, line in enumerate(lines)
                 if line.startswith('INPUT_CHAIN_CAPTURE_READY')]
    rc_prefix = '__RC%d=' % capture_ready_step
    completions = [(i, line) for i, line in enumerate(lines)
                   if line.startswith(rc_prefix)]
    if len(started) != 1 or len(witnesses) != 1 or len(completions) != 1:
        return False
    index, witness = witnesses[0]
    match = re.fullmatch(r'INPUT_CHAIN_CAPTURE_READY devices=([1-9][0-9]*)', witness)
    completion_index, completion = completions[0]
    rc_match = re.fullmatch(re.escape(rc_prefix) + r'([0-9]+)', completion)
    if (not match or len(match.group(1)) > 2 or int(match.group(1)) > 32
            or rc_match is None or rc_match.group(1) != '0'
            or not started[0] < index < completion_index):
        return False
    return not any(line.startswith('INPUT_CHAIN_CAPTURE_READY_ERROR') for line in lines)


def guarded_type(text, sender, step=3, require_input_capture_ready=False,
                 capture_ready_step=None):
    if not ready(text, step):
        raise ValueError('Refusing physical keys: missing genuine full-line readiness witnesses')
    if require_input_capture_ready and not input_capture_ready(text, capture_ready_step):
        raise ValueError('Refusing physical keys: raw input capture has no valid devices-open witness')
    return sender()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--log', required=True)
    ap.add_argument('--ready-step', type=int, default=3)
    ap.add_argument('--require-input-capture-ready', action='store_true',
                    help='diagnostic mode: fail closed unless the collector opened devices')
    ap.add_argument('--capture-ready-step', type=int,
                    help='qemu-test command index for the guest readiness helper completion')
    ap.add_argument('--qmp', required=True)
    ap.add_argument('text')
    a = ap.parse_args()
    try:
        if a.require_input_capture_ready:
            with Path(a.log).open('rb') as source:
                data = source.read(MAX_DIAGNOSTIC_LOG_BYTES + 1)
            if len(data) > MAX_DIAGNOSTIC_LOG_BYTES:
                raise ValueError('diagnostic QEMU log exceeds read bound')
            log_text = data.decode(errors='replace')
        else:
            log_text = Path(a.log).read_text(errors='replace')
        return guarded_type(log_text, lambda: subprocess.run(
            [sys.executable, str(Path(__file__).with_name('qemu-type.py')), '--qmp', a.qmp, a.text]
        ).returncode, a.ready_step, a.require_input_capture_ready, a.capture_ready_step)
    except (OSError, ValueError) as e:
        print(str(e), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
