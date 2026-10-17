#!/usr/bin/env python3
"""Check read-only hardware evdev capture against exact QMP taps, NOT GUI proof."""
import argparse
import importlib.machinery
import json
from pathlib import Path
import re

DEFAULT_TEXT = 'tty > /tmp/kms-tty; id -u > /tmp/kms-uid; echo kms-input-ok > /tmp/kms-input-ok.tmp; mv /tmp/kms-input-ok.tmp /tmp/kms-input-ok\n'
KEYS = dict(zip('qwertyuiop', range(16, 26)))
KEYS.update(zip('asdfghjkl', range(30, 39)))
KEYS.update(zip('zxcvbnm', range(44, 51)))
KEYS.update(zip('1234567890', range(2, 12)))
KEYS.update(spc=57, ret=28, minus=12, equal=13, slash=53, dot=52,
            comma=51, semicolon=39, apostrophe=40, backslash=43,
            grave_accent=41, bracket_left=26, bracket_right=27, shift=42)
M = importlib.machinery.SourceFileLoader('capture_qmp_type', str(Path(__file__).with_name('qemu-type.py'))).load_module().M
EVENT = re.compile(r'^INPUT_CAPTURE_EVENT device=(\d+) sec=(-?\d+) usec=(-?\d+) type=(\d+) code=(\d+) value=(-?\d+) received_ns=(\d+)$')
DEVICE = re.compile(r'^INPUT_CAPTURE_DEVICE index=(\d+) named=(\d+) monotonic=(\d+) name=(.*)$')
DONE = re.compile(r'^INPUT_CAPTURE_DONE events=(\d+) repeats=(\d+) dropped=(\d+) error=(\d+)$')


def expected_frames(text):
    frames = []
    for ch in text:
        k = M.get(ch) or (('shift-' + ch.lower()) if ch.isupper() else ch)
        keys = [KEYS[x] for x in k.split('-')]
        frames.append([(key, 1) for key in keys] + [(key, 0) for key in reversed(keys)])
    return frames


def check(raw, text=DEFAULT_TEXT):
    if len(raw) > 64 * 1024 * 1024:
        raise ValueError('capture log exceeds bound')
    devices, events, done = {}, [], []
    for line in raw.splitlines():
        if m := DEVICE.fullmatch(line):
            index, named, clocked, name = m.groups(); index = int(index)
            if index in devices: raise ValueError('duplicate device record')
            if named != '1' or clocked != '1': raise ValueError('unidentified/nonmonotonic device')
            devices[index] = name
        elif m := EVENT.fullmatch(line): events.append(tuple(map(int, m.groups())))
        elif m := DONE.fullmatch(line): done.append(tuple(map(int, m.groups())))
        elif line.startswith(('INPUT_CAPTURE_EVENT ', 'INPUT_CAPTURE_DEVICE ', 'INPUT_CAPTURE_DONE ')):
            raise ValueError('malformed capture record')
    if not devices or len(done) != 1:
        raise ValueError('missing devices or exactly one completion record')
    if done[0] != (len(events), 0, 0, 0):
        raise ValueError('incomplete capture, explicit repeats, dropped events or reader error')
    frames, pending, last, received, key_devices = {}, {}, {}, {}, set()
    for device, sec, usec, typ, code, value, receipt in events:
        if device not in devices: raise ValueError('event from unknown device')
        if sec < 0 or not 0 <= usec < 1000000: raise ValueError('invalid event timestamp')
        stamp = sec * 1000000 + usec
        if stamp < last.get(device, 0) or receipt < received.get(device, 0):
            raise ValueError('nonmonotonic event/receipt timestamps')
        last[device], received[device] = stamp, receipt
        if typ == 1:
            key_devices.add(device); pending.setdefault(device, []).append((code, value))
        if typ == 0 and code == 3: raise ValueError('SYN_DROPPED')
        if typ == 0 and code == 0 and pending.get(device):
            frames.setdefault(device, []).append(pending.pop(device))
    if pending: raise ValueError('key events without completing SYN_REPORT')
    if len(key_devices) != 1: raise ValueError('expected exactly one physical key stream')
    device = next(iter(key_devices)); expected = expected_frames(text)
    if frames.get(device) != expected:
        actual = frames.get(device, [])
        mismatch = next((i for i, pair in enumerate(zip(actual, expected)) if pair[0] != pair[1]), min(len(actual), len(expected)))
        raise ValueError('hardware key/frame mismatch at tap %d: got%d expected%d' % (mismatch, len(actual), len(expected)))
    return {'device': device, 'name': devices[device], 'taps': len(expected),
            'key_events': sum(map(len, expected)), 'all_events': len(events),
            'explicit_repeats': 0, 'dropped': 0,
            'scope': 'raw hardware stream only; not compositor/client or reliable GUI proof'}


def main():
    ap = argparse.ArgumentParser(description=__doc__); ap.add_argument('log', type=Path); ap.add_argument('--text', default=DEFAULT_TEXT)
    args = ap.parse_args()
    try: result = check(args.log.read_text(errors='replace'), args.text)
    except (OSError, ValueError, KeyError) as error: ap.exit(1, 'FAIL: ' + str(error) + '\n')
    print(json.dumps(result, sort_keys=True))


if __name__ == '__main__': main()
