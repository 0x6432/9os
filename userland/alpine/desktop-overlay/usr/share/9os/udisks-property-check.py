#!/usr/bin/env python3
"""Check fixed-scope dbus-send Properties.Get readbacks, not arbitrary DBus text.

The printer does not escape strings. Accept only the complete known typed body,
never search for a substring or split arbitrary strings into synthetic records.
UDisks Device and MountPoints are NUL-terminated bytestrings (ay and aay).
This disk-root smoke deliberately expects one mount point: root, not a general
mount browser. Unknown formats or additional mount points must fail closed.
"""
import re
import sys

MAX_REPLY = 65536
IDENTITIES = {'Device': ('/dev/vda', '/dev/vda1'), 'MountPoints': ('/',)}


def check_reply(property_name, expected, reply):
    if property_name not in IDENTITIES:
        raise ValueError('unsupported property')
    if expected not in IDENTITIES[property_name]:
        raise ValueError('unsupported identity')
    header, separator, body = reply.partition('\n')
    if not separator or not header.startswith('method return '):
        raise ValueError('not a method return')
    value = r'array of bytes "' + re.escape(expected) + r'" \+ \\0'
    if property_name == 'MountPoints':
        value = r'array \[\s*' + value + r'\s*\]'
    if not re.fullmatch(r'\s*variant\s+' + value + r'\s*', body):
        raise ValueError('unexpected typed property value')


def main():
    try:
        if len(sys.argv) != 4:
            raise ValueError('expected property, identity and reply file')
        with open(sys.argv[3], 'rb') as stream:
            reply = stream.read(MAX_REPLY + 1)
        if len(reply) > MAX_REPLY:
            raise ValueError('oversized property reply')
        check_reply(sys.argv[1], sys.argv[2], reply.decode('ascii'))
    except (OSError, UnicodeError, ValueError) as error:
        print('alpine-udisks: property readback rejected: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
