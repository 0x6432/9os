#!/usr/bin/env python3
"""Offline-only extraction of complete captured replies; never executes DBus.

Use separate original command logs or allow numbered records from combined job
logs. No file is produced for incomplete/unknown/failed-command evidence.
"""
import argparse
from pathlib import Path
import re

CALLS = {
    '1': ('/org/freedesktop/UDisks2/block_devices/vda', 'org.freedesktop.UDisks2.Block', 'Device'),
    '2': ('/org/freedesktop/UDisks2/block_devices/vda1', 'org.freedesktop.UDisks2.Block', 'Device'),
    '3': ('/org/freedesktop/UDisks2/block_devices/vda1', 'org.freedesktop.UDisks2.Filesystem', 'MountPoints'),
}
FIELDS = set(('call path interface property bus destination method command_status '
              'checker_status recording typed capture complete truncated unknown '
              'captured_bytes data_hex').split())


def parse_line(line):
    # Accommodate a GitHub timestamp prefix, not arbitrary prompts/embedded text.
    line = line.rstrip('\r\n')
    line = re.sub(r'^\d{4}-\d\d-\d\dT[0-9:.]+Z ', '', line)
    if not line.startswith('udisks_readback_v1 '):
        return None
    if len(line.encode('ascii')) + 1 > 3072:
        raise ValueError('record bound')
    pairs = [part.split('=', 1) for part in line.split()[1:]]
    if any(len(pair) != 2 for pair in pairs):
        raise ValueError('fields')
    fields = dict(pairs)
    if set(fields) != FIELDS or len(pairs) != len(FIELDS):
        raise ValueError('missing/duplicate fields')
    call = fields['call']
    if call not in CALLS or tuple(fields[k] for k in ('path', 'interface', 'property')) != CALLS[call]:
        raise ValueError('call identity')
    if (fields['bus'], fields['destination'], fields['method']) != (
            'system', 'org.freedesktop.UDisks2', 'org.freedesktop.DBus.Properties.Get'):
        raise ValueError('method identity')
    for key in ('command_status', 'checker_status'):
        if key == 'checker_status' and fields[key] == 'unknown':
            continue
        if not re.fullmatch(r'0|[1-9][0-9]{0,2}', fields[key]) or int(fields[key]) > 255:
            raise ValueError('status')
    if fields['recording'] not in ('ok', 'error') or fields['typed'] not in ('passed', 'failed', 'unknown'):
        raise ValueError('outcome')
    if fields['capture'] not in ('complete', 'truncated', 'empty', 'unknown'):
        raise ValueError('capture')
    if any(fields[k] not in ('0', '1', 'unknown') for k in ('complete', 'truncated', 'unknown')):
        raise ValueError('flags')
    data = fields['data_hex']
    if data in ('empty', 'unknown'):
        decoded = b'' if data == 'empty' else None
    elif re.fullmatch(r'(?:[0-9a-f]{2}){1,1024}', data):
        decoded = bytes.fromhex(data)
    else:
        raise ValueError('lowercase hex')
    if decoded is None:
        if fields['captured_bytes'] != 'unknown':
            raise ValueError('unknown length')
    elif fields['captured_bytes'] != str(len(decoded)):
        raise ValueError('length')
    if fields['complete'] == '1':
        if not (fields['command_status'] == '0' and fields['recording'] == 'ok' and
                fields['capture'] == 'complete' and fields['truncated'] == '0' and
                fields['unknown'] == '0' and decoded):
            raise ValueError('fabricated completeness')
        return fields, decoded
    return fields, None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    # Parse everything before creating output; malformed records are never saved.
    records = [record for line in args.log.read_text(encoding='utf-8', errors='replace').splitlines()
               if (record := parse_line(line)) is not None]
    if not records:
        parser.error('no readback records; no evidence bytes reconstructed')
    args.output_dir.mkdir(exist_ok=False)
    for index, (fields, data) in enumerate(records, 1):
        if data is None:
            print(f'record={index} call={fields["call"]} capture={fields["capture"]} no_complete_reply')
            continue
        filename = f'{index:04d}-call-{fields["call"]}.reply'
        with (args.output_dir / filename).open('xb') as stream:
            stream.write(data)
        print(f'record={index} call={fields["call"]} typed={fields["typed"]} bytes={len(data)} file={filename}')


if __name__ == '__main__':
    main()
