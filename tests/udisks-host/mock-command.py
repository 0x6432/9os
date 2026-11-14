#!/usr/bin/env python3
"""Command-only fixtures. Never start or emulate a DBus service."""
import json
import os
from pathlib import Path
import sys

name = Path(sys.argv[0]).name
args = sys.argv[1:]
config = json.loads(Path(os.environ['UDISKS_TEST_CONFIG']).read_text())
if name == 'id':
    assert args == ['-u'], args
    print(config.get('uid', '1000'))
elif name == 'timeout':
    assert args[:4] in ([ '-s', 'KILL', '40', 'dbus-send'],
                       [ '-s', 'KILL', '6', 'dbus-send']), args
    assert args[2] == ('40' if args[-1] ==
                       'org.freedesktop.DBus.ObjectManager.GetManagedObjects' else '6'), args
    os.execvp(args[3], args[3:])
elif name == 'dbus-send':
    log = Path(os.environ['UDISKS_TEST_LOG'])
    calls = log.read_text().splitlines() if log.exists() else []
    with log.open('a') as stream:
        stream.write(json.dumps(args) + '\n')
    response = config['replies'][len(calls)]
    sys.stdout.write(response.get('stdout', ''))
    sys.stderr.write(response.get('stderr', ''))
    sys.exit(response.get('status', 0))
else:
    raise AssertionError(name)
