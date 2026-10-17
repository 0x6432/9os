#!/usr/bin/env python3
"""Check exact stock libwayland 1.23.1 pre-listener keyboard dispatch.

This is NOT a GUI pass or proof of Qt event handling. WAYLAND_DEBUG uses a
wrapping uint32 CLOCK_REALTIME microsecond value; never treat it as monotonic.
"""
from pathlib import Path
from importlib.machinery import SourceFileLoader
import argparse
import json
import re

CAPTURE = SourceFileLoader('dispatch_capture', str(Path(__file__).with_name('input-capture-check.py'))).load_module()
KEY = re.compile(r'^\[\s*(\d+)\.(\d{3})\] \{([^{}]+)\} wl_keyboard#(\d+)\.key\((\d+), (\d+), (\d+), ([01])\)$')
REPEAT = re.compile(r'^\[\s*(\d+)\.(\d{3})\] \{([^{}]+)\} wl_keyboard#(\d+)\.repeat_info\((-?\d+), (-?\d+)\)$')


def debug_clock(whole, fraction):
    if int(whole)*1000 + int(fraction) > 0xffffffff:
        raise ValueError('debug clock outside uint32 microseconds')


def check(raw):
    if len(raw) > 64*1024*1024:
        raise ValueError('log exceeds bound')
    blocks = re.findall(r'^INNER_KEYBOARD_DISPATCH_TRACE_BEGIN\n(.*?)^INNER_KEYBOARD_DISPATCH_TRACE_END$', raw, re.M | re.S)
    if not blocks:
        raise ValueError('no complete framed inner keyboard dispatch trace')
    if any(block != blocks[0] for block in blocks):
        raise ValueError('ambiguous nonidentical trace replays')
    rows, objects, queues, repeats = [], set(), set(), []
    for line in blocks[0].splitlines():
        if re.search(r'wl_keyboard[#@]\d+\.key\(', line):
            match = KEY.fullmatch(line)
            if not match:
                raise ValueError('malformed, discarded or outgoing key record')
            whole, fraction, queue, obj, serial, stamp, code, state = match.groups()
            debug_clock(whole, fraction)
            if any(int(value) > 0xffffffff for value in (obj, serial, stamp, code)):
                raise ValueError('event unsigned field outside uint32')
            objects.add(int(obj)); queues.add(queue); rows.append((int(code), int(state)))
        elif 'repeat_info(' in line:
            match = REPEAT.fullmatch(line)
            if not match:
                raise ValueError('malformed repeat announcement')
            whole, fraction, queue, obj, rate, delay = match.groups()
            debug_clock(whole, fraction)
            if int(obj) > 0xffffffff or any(not -0x80000000 <= int(v) <= 0x7fffffff for v in (rate, delay)):
                raise ValueError('repeat field outside protocol range')
            repeats.append((int(obj), queue, int(rate), int(delay)))
    expected = [edge for frame in CAPTURE.expected_frames(CAPTURE.DEFAULT_TEXT) for edge in frame]
    if len(objects) != 1 or len(queues) != 1 or rows != expected:
        mismatch = next((i for i, pair in enumerate(zip(rows, expected)) if pair[0] != pair[1]), min(len(rows), len(expected)))
        raise ValueError('inner key mismatch at edge%d got%d expected%d objects%d queues%d' % (mismatch, len(rows), len(expected), len(objects), len(queues)))
    if not repeats or len(set(repeats)) != 1 or any(obj not in objects or queue not in queues for obj, queue, _, _ in repeats):
        raise ValueError('missing, mixed-object/queue or changing repeat configuration')
    return {'key_edges': len(rows), 'intended_taps': len(CAPTURE.DEFAULT_TEXT), 'identical_log_replays': len(blocks),
            'repeat_announcements': len(repeats), 'repeat_rate': repeats[0][2], 'repeat_delay_ms': repeats[0][3],
            'scope': 'libwayland pre-listener dispatch only; not Qt handling, autorepeat, timing or GUI acceptance proof'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    try:
        result = check(args.log.read_text(errors='replace'))
    except (OSError, ValueError) as error:
        parser.exit(1, 'FAIL: ' + str(error) + '\n')
    print(json.dumps(result, sort_keys=True))


if __name__ == '__main__':
    main()
