#!/usr/bin/env python3
"""Check one retained KMS attempt across raw, outer, inner, and Qt input traces.

This is a diagnostic correlation check, not a desktop acceptance or reliability
test. The original QEMU gates remain authoritative.
"""
from importlib.machinery import SourceFileLoader
from pathlib import Path
import argparse
import json
import re

HERE = Path(__file__).resolve().parent
RAW = SourceFileLoader('chain_raw', str(HERE / 'input-capture-check.py')).load_module()
INNER = SourceFileLoader('chain_inner', str(HERE / 'inner-keyboard-dispatch-check.py')).load_module()
QT = SourceFileLoader('chain_qt', str(HERE / 'qt-key-observer-check.py')).load_module()

MAX_TRANSCRIPT = 256 * 1024 * 1024
OUTER_BEGIN = 'OUTER_KEYBOARD_TRACE_BEGIN'
OUTER_END = 'OUTER_KEYBOARD_TRACE_END'
RAW_BEGIN = 'INPUT_CHAIN_RAW_CAPTURE_BEGIN'
RAW_END = 'INPUT_CHAIN_RAW_CAPTURE_END'
HEADER = re.compile(r'^WAYLAND_KEY_TRACE object=(\d+) event=([a-z_]+) received_ns=(\d+) (.+)$')
KEY = re.compile(r'^serial=(\d+) time_ms=(\d+) key=(\d+) state=([01])$')
REPEAT = re.compile(r'^rate=(-?\d+) delay=(-?\d+)$')
ENTER = re.compile(r'^serial=(\d+) surface=(\d+) held_keys=(\d+)$')
LEAVE = re.compile(r'^serial=(\d+) surface=(\d+)$')
READY = re.compile(r'^INPUT_CAPTURE_READY devices=([1-9][0-9]*)$')
CHAIN_READY = re.compile(r'^INPUT_CHAIN_CAPTURE_READY devices=([1-9][0-9]*)$')
U64_MAX = 0xffffffffffffffff
S64_MIN = -(1 << 63)
S64_MAX = (1 << 63) - 1
S32_MIN = -(1 << 31)
S32_MAX = (1 << 31) - 1
U16_MAX = 0xffff


def _one_block(raw, begin, end):
    pattern = re.compile(r'^' + re.escape(begin) + r'\n(.*?)^' + re.escape(end) + r'$', re.M | re.S)
    blocks = pattern.findall(raw)
    starts = len(re.findall(r'^' + re.escape(begin) + r'$', raw, re.M))
    ends = len(re.findall(r'^' + re.escape(end) + r'$', raw, re.M))
    if starts != 1 or ends != 1 or len(blocks) != 1:
        raise ValueError('missing, duplicate, or incomplete ' + begin + ' block')
    return begin + '\n' + blocks[0] + end + '\n', blocks[0]


def _u32(value, field):
    number = int(value)
    if not 0 <= number <= 0xffffffff:
        raise ValueError('outer ' + field + ' outside uint32')
    return number


def _u64(value, field):
    number = int(value)
    if not 0 <= number <= U64_MAX:
        raise ValueError('outer ' + field + ' outside uint64')
    return number


def check_outer(block):
    lines = block.splitlines()
    if not lines or len(lines) > 4096:
        raise ValueError('empty or oversized outer trace')
    edges, objects, repeats = [], set(), []
    last_received = 0
    for line in lines:
        if line == 'WAYLAND_KEY_TRACE truncated=1':
            raise ValueError('outer trace explicitly truncated')
        match = HEADER.fullmatch(line)
        if not match:
            raise ValueError('malformed/unexpected outer trace record')
        obj, event, received_ns, payload = match.groups()
        obj = _u32(obj, 'object')
        received_ns = _u64(received_ns, 'receipt time')
        if received_ns <= 0 or received_ns < last_received:
            raise ValueError('invalid or nonmonotonic outer receipt time')
        last_received = received_ns
        objects.add(obj)
        if event == 'key':
            fields = KEY.fullmatch(payload)
            if not fields:
                raise ValueError('malformed outer key record')
            serial, stamp, code, state = fields.groups()
            _u32(serial, 'serial')
            _u32(stamp, 'source time')
            _u32(code, 'key')
            edges.append((int(code), int(state)))
        elif event == 'repeat_info':
            fields = REPEAT.fullmatch(payload)
            if not fields:
                raise ValueError('malformed outer repeat record')
            rate, delay = map(int, fields.groups())
            if any(not -0x80000000 <= item <= 0x7fffffff for item in (rate, delay)):
                raise ValueError('outer repeat field outside int32')
            repeats.append((obj, rate, delay))
        elif event == 'enter':
            fields = ENTER.fullmatch(payload)
            if not fields:
                raise ValueError('malformed outer enter record')
            for name, value in zip(('serial', 'surface', 'held_keys'), fields.groups()):
                _u32(value, name)
        elif event == 'leave':
            fields = LEAVE.fullmatch(payload)
            if not fields:
                raise ValueError('malformed outer leave record')
            for name, value in zip(('serial', 'surface'), fields.groups()):
                _u32(value, name)
        else:
            raise ValueError('unknown outer trace event')

    expected = [edge for frame in RAW.expected_frames(RAW.DEFAULT_TEXT) for edge in frame]
    if len(objects) != 1 or not edges or not repeats:
        raise ValueError('outer trace missing one keyboard stream or repeat settings')
    if any(obj not in objects for obj, _, _ in repeats) or len(set(repeats)) != 1:
        raise ValueError('outer repeat settings are mixed or changing')
    if edges != expected:
        mismatch = next((i for i, pair in enumerate(zip(edges, expected)) if pair[0] != pair[1]),
                        min(len(edges), len(expected)))
        raise ValueError('outer key mismatch at edge%d got%d expected%d' %
                         (mismatch, len(edges), len(expected)))
    return {'key_edges': len(edges), 'intended_taps': len(RAW.expected_frames(RAW.DEFAULT_TEXT)),
            'repeat_rate': repeats[0][1], 'repeat_delay_ms': repeats[0][2],
            'scope': 'outer compositor-to-KWin wire observation before forwarding'}


def validate_raw_ready(raw_lines):
    devices = set()
    ready_counts = []
    done_records = []
    for line in raw_lines.splitlines():
        if line.startswith('INPUT_CAPTURE_ERROR'):
            raise ValueError('raw collector reported an error')
        if line.startswith('INPUT_CAPTURE_READY'):
            match = READY.fullmatch(line)
            if not match:
                raise ValueError('malformed raw collector READY record')
            if len(match.group(1)) > 2 or int(match.group(1)) > 32:
                raise ValueError('raw READY device count exceeds collector limit')
            ready_counts.append(int(match.group(1)))
        elif line.startswith('INPUT_CAPTURE_DEVICE'):
            match = RAW.DEVICE.fullmatch(line)
            if not match:
                raise ValueError('malformed raw collector DEVICE record')
            index, named, monotonic, _ = match.groups()
            index = int(index)
            if not 0 <= index <= 31 or named != '1' or monotonic != '1' or index in devices:
                raise ValueError('invalid, duplicate, or unsupported raw device record')
            devices.add(index)
        elif line.startswith('INPUT_CAPTURE_EVENT'):
            match = RAW.EVENT.fullmatch(line)
            if not match:
                raise ValueError('malformed raw collector EVENT record')
            device, sec, usec, event_type, code, value, received_ns = map(int, match.groups())
            if not 0 <= device <= 31:
                raise ValueError('raw device index outside collector range')
            if not S64_MIN <= sec <= S64_MAX or not S64_MIN <= usec <= S64_MAX:
                raise ValueError('raw event timestamp outside signed64 ABI')
            if not 0 <= usec < 1000000:
                raise ValueError('raw event usec outside valid timeval range')
            if not 0 <= event_type <= U16_MAX or not 0 <= code <= U16_MAX:
                raise ValueError('raw event type/code outside uint16 ABI')
            if not S32_MIN <= value <= S32_MAX:
                raise ValueError('raw event value outside signed32 ABI')
            if not 0 <= received_ns <= U64_MAX:
                raise ValueError('raw receipt time outside uint64')
        elif line.startswith('INPUT_CAPTURE_DONE'):
            match = RAW.DONE.fullmatch(line)
            if not match:
                raise ValueError('malformed raw collector DONE record')
            done_records.append(tuple(map(int, match.groups())))
    if len(ready_counts) != 1:
        raise ValueError('raw capture requires exactly one valid READY record')
    if not devices or ready_counts[0] != len(devices):
        raise ValueError('raw READY device count does not match valid device records')
    if len(done_records) != 1:
        raise ValueError('raw capture requires exactly one valid DONE record')
    return ready_counts[0]


def check(raw):
    if len(raw) > MAX_TRANSCRIPT:
        raise ValueError('input-chain transcript exceeds bound')
    if len(re.findall(r'^INPUT_CHAIN_DIAGNOSTICS_ENABLED$', raw, re.M)) != 1:
        raise ValueError('missing or duplicate diagnostic-mode witness')
    started_lines = [i for i, line in enumerate(raw.splitlines())
                     if line == 'INPUT_CHAIN_CAPTURE_STARTED seconds=45']
    if len(started_lines) != 1:
        raise ValueError('raw capture was not started exactly once')
    transcript_lines = raw.splitlines()
    chain_ready_lines = [(i, line) for i, line in enumerate(transcript_lines)
                         if line.startswith('INPUT_CHAIN_CAPTURE_READY')]
    chain_ready = [(i, CHAIN_READY.fullmatch(line)) for i, line in chain_ready_lines]
    if len(chain_ready) != 1:
        raise ValueError('missing or duplicate devices-open readiness witness')
    if chain_ready[0][1] is None:
        raise ValueError('malformed devices-open readiness witness')
    ready_line, ready_match = chain_ready[0]
    if started_lines[0] >= ready_line:
        raise ValueError('devices-open readiness witness precedes collector start')
    if len(ready_match.group(1)) > 2 or int(ready_match.group(1)) > 32:
        raise ValueError('devices-open readiness count exceeds collector limit')
    if any(line.startswith('INPUT_CHAIN_CAPTURE_READY_ERROR') for line in transcript_lines):
        raise ValueError('devices-open readiness reported a failure')

    outer_block, outer_body = _one_block(raw, OUTER_BEGIN, OUTER_END)
    inner_block, _ = _one_block(raw, 'INNER_KEYBOARD_DISPATCH_TRACE_BEGIN',
                                'INNER_KEYBOARD_DISPATCH_TRACE_END')
    qt_block, _ = _one_block(raw, QT.START, QT.END)
    _, raw_body = _one_block(raw, RAW_BEGIN, RAW_END)

    status = re.findall(r'^INPUT_CHAIN_RAW_STATUS=([^\s]+)$', raw_body, re.M)
    if status != ['0']:
        raise ValueError('raw capture completion status missing or nonzero')
    raw_lines = '\n'.join(line for line in raw_body.splitlines()
                          if line.startswith(('INPUT_CAPTURE_EVENT', 'INPUT_CAPTURE_DEVICE',
                                              'INPUT_CAPTURE_DONE', 'INPUT_CAPTURE_READY',
                                              'INPUT_CAPTURE_ERROR')))
    raw_device_count = validate_raw_ready(raw_lines)
    if raw_device_count != int(ready_match.group(1)):
        raise ValueError('host and retained raw readiness device counts differ')
    raw_result = RAW.check(raw_lines)
    outer_result = check_outer(outer_body)
    inner_result = INNER.check(inner_block)
    qt_result = QT.check(qt_block)
    if qt_result['autorepeat_events'] != 0:
        raise ValueError('Qt platform trace contains autorepeat')
    expected_edges = sum(len(frame) for frame in RAW.expected_frames(RAW.DEFAULT_TEXT))
    for name, result, key in (
            ('raw', raw_result, 'key_events'),
            ('outer', outer_result, 'key_edges'),
            ('inner', inner_result, 'key_edges'),
            ('Qt', qt_result, 'physical_key_edges')):
        if result[key] != expected_edges:
            raise ValueError('%s edge count %d differs from expected %d' %
                             (name, result[key], expected_edges))
    return {'raw': raw_result, 'outer': outer_result, 'inner': inner_result, 'qt': qt_result,
            'scope': ('orchestrated one-run artifact only; producer provenance is not authenticated and '
                      'spliced-log rejection is not provided; not terminal-consumption or cold-reliability proof')}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path, help='unique host log from one KMS attempt')
    args = parser.parse_args()
    try:
        if args.log.stat().st_size > MAX_TRANSCRIPT:
            raise ValueError('input-chain transcript exceeds bound')
        result = check(args.log.read_text(errors='replace'))
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, 'FAIL: ' + str(error) + '\n')
    print(json.dumps(result, sort_keys=True))


if __name__ == '__main__':
    main()
