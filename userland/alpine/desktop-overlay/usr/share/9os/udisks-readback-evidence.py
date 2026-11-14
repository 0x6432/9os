#!/usr/bin/env python3
"""Bounded diagnostic bytes for only the three existing UDisks Get readbacks.

No DBus operations or acceptance decisions. Data is lowercase hex; metadata is
selected from fixed constants/statuses. A failed command never proves a complete
reply, even if its redirected file has EOF. Capture errors are explicit unknowns.
"""
import sys

MAX_CAPTURE = 1024
MAX_RECORD = 3072
CALLS = {
    '1': ('/org/freedesktop/UDisks2/block_devices/vda',
          'org.freedesktop.UDisks2.Block', 'Device'),
    '2': ('/org/freedesktop/UDisks2/block_devices/vda1',
          'org.freedesktop.UDisks2.Block', 'Device'),
    '3': ('/org/freedesktop/UDisks2/block_devices/vda1',
          'org.freedesktop.UDisks2.Filesystem', 'MountPoints'),
}


def status(value, allow_unknown=False):
    if allow_unknown and value == 'unknown':
        return value
    if not value.isascii() or not value.isdigit() or not 0 <= int(value) <= 255:
        raise ValueError('status')
    return str(int(value))


def record(call, command_status, checker_status, path):
    object_path, interface, prop = CALLS[call]
    command_status = status(command_status)
    checker_status = status(checker_status, allow_unknown=True)
    if command_status != '0' and checker_status != 'unknown':
        raise ValueError('checker must not run after command failure')
    typed = ('unknown' if checker_status == 'unknown' else
             'passed' if checker_status == '0' else 'failed')
    recording = 'ok'
    capture = complete = truncated = 'unknown'
    unknown = '1'
    captured_bytes = data_hex = 'unknown'
    try:
        with open(path, 'rb') as stream:
            data = stream.read(MAX_CAPTURE + 1)
        overflow = len(data) > MAX_CAPTURE
        prefix = data[:MAX_CAPTURE]
        captured_bytes = str(len(prefix))
        data_hex = prefix.hex() if prefix else 'empty'
        if command_status != '0':
            # Actual captured prefix only; failed process completion is not a
            # claim that this is a complete DBus response or a zero-length one.
            truncated = '1' if overflow else 'unknown'
        elif overflow:
            capture, complete, truncated = 'truncated', '0', '1'
        elif not prefix:
            capture, complete, truncated = 'empty', '0', '0'
        else:
            capture, complete, truncated, unknown = 'complete', '1', '0', '0'
    except OSError:
        recording = 'error'
    line = (f'udisks_readback_v1 call={call} path={object_path} interface={interface} '
            f'property={prop} bus=system destination=org.freedesktop.UDisks2 '
            f'method=org.freedesktop.DBus.Properties.Get '
            f'command_status={command_status} checker_status={checker_status} '
            f'recording={recording} typed={typed} capture={capture} complete={complete} '
            f'truncated={truncated} unknown={unknown} captured_bytes={captured_bytes} '
            f'data_hex={data_hex}\n')
    if len(line.encode('ascii')) > MAX_RECORD:
        raise ValueError('record bound')
    return line


def main():
    try:
        if len(sys.argv) != 5:
            raise ValueError('arguments')
        line = record(*sys.argv[1:])
        sys.stdout.write(line)
        sys.stdout.flush()
        return 0
    except (OSError, ValueError, KeyError):
        # Never replay exception/filename/raw bytes into the host FAIL sentinel.
        try:
            sys.stderr.write('udisks_readback_recording_error\n')
        except OSError:
            pass
        return 1


if __name__ == '__main__':
    sys.exit(main())
