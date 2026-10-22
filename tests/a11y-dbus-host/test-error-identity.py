#!/usr/bin/env python3
"""Tiny native opaque-error injection controls; NOT IPC/GUI/cold evidence."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
TOP = HERE.parents[1]
SOURCE = Path(os.environ.get('A11Y_ERROR_SOURCE', TOP / 'userland/tests/a11y-dbus/terminal-dbus.c'))
OPTIONAL = ['g_error_matches', 'g_io_error_quark', 'g_dbus_error_is_remote_error',
            'g_dbus_error_get_remote_error', 'g_free']
CAPS = {'method': 32, 'bus': 256, 'path': 1024, 'remote_name': 256}


def parse_records(text):
    records = []
    for line in text.splitlines():
        if not line.startswith('DBUS_A11Y_ERROR_IDENTITY '):
            continue
        assert len(line.encode()) + 1 < 3584, line
        assert not re.search(r'FAIL|panic|__RC[0-9]+=', line), line
        fields = dict(item.split('=', 1) for item in line.split()[1:])
        assert set(fields) == {'class', 'remote_name_status'} | {
            name + suffix for name in CAPS for suffix in ('_hex', '_truncated')}, fields
        assert fields['class'] in ('local-timeout', 'remote-dbus', 'other', 'unknown'), fields
        assert fields['remote_name_status'] in ('present', 'absent', 'unknown'), fields
        for name, cap in CAPS.items():
            value = fields[name + '_hex']
            assert re.fullmatch(r'[0-9a-f]*', value) and len(value) % 2 == 0, fields
            assert len(value) <= cap * 2, fields
            assert fields[name + '_truncated'] in ('0', '1'), fields
        records.append(fields)
    assert len(records) <= 2, records
    return records


class Identity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='a11y-error-model-')
        cls.binary = Path(cls.temp.name) / 'model'
        flags = shlex.split(subprocess.check_output(
            ['pkg-config', '--cflags', '--libs', 'gio-2.0'], text=True))
        subprocess.run([os.environ.get('CC', 'cc'), '-O2', '-Wall', '-Wextra', '-Werror',
                        '-D_FORTIFY_SOURCE=2', '-DIDENTITY_CONSUMER_SOURCE="' + str(SOURCE) + '"',
                        str(HERE / 'error-identity-model.c'),
                        *flags, '-ldl', '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_model(self, mode, hide=None):
        r = subprocess.run([str(self.binary), mode] + ([hide] if hide else []),
                           text=True, capture_output=True, timeout=5)
        self.assertEqual(r.returncode, 1, (r.stdout, r.stderr))
        self.assertEqual(r.stderr, '')
        self.assertNotIn('UNPRINTED_SECRET', r.stdout)
        self.assertIn('DBUS_A11Y_ERROR reason=remote-call\n', r.stdout)
        self.assertNotIn('DBUS_A11Y_WITNESS', r.stdout)
        identities = parse_records(r.stdout)
        self.assertEqual(len(identities), 2 if mode in ('long', 'async-both') else 1, r.stdout)
        lines = r.stdout.splitlines()
        for i, line in enumerate(lines):
            if line.startswith('DBUS_A11Y_ERROR_IDENTITY '):
                self.assertEqual(lines[i-1], 'DBUS_A11Y_ERROR reason=remote-call')
        self.assertEqual(r.stdout.count('DBUS_A11Y_ERROR reason=remote-call\n'),
                         3 if mode == 'long' else 2 if mode == 'async-both' else 1)
        return identities

    def test_real_local_timeout(self):
        record, = self.run_model('local-timeout')
        self.assertEqual(record['class'], 'local-timeout')
        self.assertEqual(record['remote_name_status'], 'absent')
        self.assertEqual(record['remote_name_hex'], '')
        self.assertEqual(bytes.fromhex(record['method_hex']), b'GetAddress')
        self.assertEqual(bytes.fromhex(record['bus_hex']), b'org.a11y.Bus')
        self.assertEqual(bytes.fromhex(record['path_hex']), b'/org/a11y/bus')

    def test_real_remote_name(self):
        record, = self.run_model('remote')
        self.assertEqual(record['class'], 'remote-dbus')
        self.assertEqual(record['remote_name_status'], 'present')
        self.assertEqual(bytes.fromhex(record['remote_name_hex']), b'org.test.FAIL')

    def test_remote_noreply_is_not_local_timeout(self):
        record, = self.run_model('remote-noreply')
        self.assertEqual(record['class'], 'remote-dbus')
        self.assertEqual(bytes.fromhex(record['remote_name_hex']), b'org.freedesktop.DBus.Error.NoReply')

    def test_other_local_error(self):
        record, = self.run_model('other')
        self.assertEqual(record['class'], 'other')
        self.assertEqual(record['remote_name_status'], 'absent')

    def test_each_missing_optional_symbol_preserves_error_as_unknown(self):
        for mode in ('remote', 'local-timeout'):
            for symbol in OPTIONAL:
                with self.subTest(mode=mode, symbol=symbol):
                    record, = self.run_model(mode, symbol)
                    self.assertEqual(record['class'], 'unknown')
                    self.assertEqual(record['remote_name_status'], 'unknown')
                    self.assertEqual(record['remote_name_hex'], '')

    def test_both_async_callbacks_correlate_and_drain_in_reverse_order(self):
        children, role = self.run_model('async-both')
        self.assertEqual(children['class'], 'remote-dbus')
        self.assertEqual(role['class'], 'local-timeout')
        self.assertEqual(bytes.fromhex(children['method_hex']), b'GetChildren')
        self.assertEqual(bytes.fromhex(role['method_hex']), b'GetRole')
        for record in (children, role):
            self.assertEqual(bytes.fromhex(record['bus_hex']), b':1.42')
            self.assertEqual(bytes.fromhex(record['path_hex']), b'/org/a11y/atspi/accessible/FAIL')

    def test_caps_truncation_and_direct_reference(self):
        first, second = self.run_model('long')
        for record in (first, second):
            self.assertEqual(record['class'], 'remote-dbus')
            self.assertEqual(bytes.fromhex(record['remote_name_hex']), b'org.test.FAIL' + b'A' * (256 - 13))
            self.assertEqual(record['remote_name_truncated'], '1')
        self.assertEqual(bytes.fromhex(second['method_hex']), b'MethodFAILabcdefghijklmnopqrstuvwxyz0123456789'[:32])
        self.assertEqual(second['method_truncated'], '1')
        self.assertEqual(bytes.fromhex(second['bus_hex']), b'B' * 255)
        self.assertEqual(second['bus_truncated'], '0')
        self.assertEqual(bytes.fromhex(second['path_hex']), b'C' * 1023)
        self.assertEqual(second['path_truncated'], '0')


if __name__ == '__main__':
    print('consumer_source_sha256=' + hashlib.sha256(SOURCE.read_bytes()).hexdigest(), flush=True)
    unittest.main(verbosity=2)
