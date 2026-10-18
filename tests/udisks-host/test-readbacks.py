#!/usr/bin/env python3
"""Native, command-mocked tests for the *real* overlay's read-only consumer."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOP = Path(__file__).resolve().parents[2]
OVERLAY = TOP / 'userland/alpine/desktop-overlay'
PROBE = OVERLAY / 'usr/bin/alpine-udisks-smoke'
CHECK = OVERLAY / 'usr/share/9os/udisks-property-check.py'
SPEC = importlib.util.spec_from_file_location('udisks_property_check', CHECK)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
HEADER = 'method return time=1.234 sender=:1.7 -> destination=:1.8 serial=19 reply_serial=2\n'
PREFIX = '/org/freedesktop/UDisks2'
COMMON = ['--system', '--print-reply']
ENUM_CALL = COMMON + ['--reply-timeout=30000', '--dest=org.freedesktop.UDisks2',
                      PREFIX, 'org.freedesktop.DBus.ObjectManager.GetManagedObjects']


def get_call(device, interface, prop):
    return COMMON + ['--reply-timeout=4000', '--dest=org.freedesktop.UDisks2',
                     PREFIX + '/block_devices/' + device,
                     'org.freedesktop.DBus.Properties.Get',
                     'string:org.freedesktop.UDisks2.' + interface, 'string:' + prop]


CALLS = [ENUM_CALL, get_call('vda', 'Block', 'Device'),
         get_call('vda1', 'Block', 'Device'), get_call('vda1', 'Filesystem', 'MountPoints')]


def byte_string(value):
    return 'array of bytes "' + value + '" + \\0'


def device_reply(value):
    # dbus-send indents the inner variant separately on the same line.
    return HEADER + '   variant       ' + byte_string(value) + '\n'


def mount_reply(value='/'):
    return HEADER + '   variant       array [\n         ' + byte_string(value) + '\n      ]\n'


# Deliberately includes all original gate fragments. A later scoped failure must
# not be hidden by these fragments, even though the original greps all succeed.
ENUM = HEADER + '''   array [
      dict entry(
         object path "/org/freedesktop/UDisks2/Manager"
         array [
            dict entry(
               string "org.freedesktop.UDisks2.Manager"
               array [
               ]
            )
         ]
      ),      dict entry(
         object path "/org/freedesktop/UDisks2/block_devices/vda"
         array [
            dict entry(
               string "org.freedesktop.UDisks2.Block"
               array [
                  dict entry(
                     string "Device"
                     variant                         array of bytes "/dev/vda" + \\0
                  )
               ]
            )
         ]
      ),      dict entry(
         object path "/org/freedesktop/UDisks2/block_devices/vda1"
         array [
            dict entry(
               string "org.freedesktop.UDisks2.Block"
               array [
                  dict entry(
                     string "Device"
                     variant                         array of bytes "/dev/vda1" + \\0
                  )
               ]
            ),            dict entry(
               string "org.freedesktop.UDisks2.Filesystem"
               array [
                  dict entry(
                     string "MountPoints"
                     variant                         array [
                           array of bytes "/" + \\0
                        ]
                  )
               ]
            )
         ]
      )
   ]
'''


def good_config():
    return {'replies': [{'stdout': text} for text in
                        (ENUM, device_reply('/dev/vda'), device_reply('/dev/vda1'), mount_reply())]}


class ReadbackTests(unittest.TestCase):
    def run_probe(self, config):
        with tempfile.TemporaryDirectory(prefix='udisks-host-') as temp:
            root = Path(temp)
            for name in ('dbus-send', 'timeout', 'id'):
                (root / name).symlink_to(Path(__file__).with_name('mock-command.py'))
            probe = root / 'alpine-udisks-smoke'
            # Change only the installed helper path, not any runtime probe logic.
            probe.write_text(PROBE.read_text().replace('/usr/share/9os/udisks-property-check.py', str(CHECK)))
            config_path, log = root / 'config.json', root / 'calls.jsonl'
            config_path.write_text(json.dumps(config))
            env = dict(os.environ, PATH=str(root) + ':' + os.environ['PATH'],
                       UDISKS_TEST_CONFIG=str(config_path), UDISKS_TEST_LOG=str(log))
            result = subprocess.run(['sh', str(probe)], env=env, text=True,
                                    capture_output=True, timeout=10)
            calls = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
        # Every path must stop at the first failing step; no retries, mutators,
        # root commands, changed destinations or unexpected methods are allowed.
        self.assertEqual(calls, CALLS[:len(calls)])
        if result.returncode:
            self.assertNotIn('real disk/partition/root filesystem enumerated', result.stdout)
        return result, calls

    def test_positive_calls_and_uid(self):
        result, calls = self.run_probe(good_config())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, CALLS)
        self.assertEqual(result.stdout, 'alpine-udisks: real disk/partition/root filesystem enumerated uid=1000\n')

    def test_wrong_uid_never_contacts_bus(self):
        for uid in ('0', '999', '1001'):
            with self.subTest(uid=uid):
                config = good_config()
                config['uid'] = uid
                result, calls = self.run_probe(config)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('requires uid=1000', result.stderr)
                self.assertEqual(calls, [])

    def test_original_gates_remain_mandatory(self):
        fragments = [
            'object path "' + PREFIX + '/Manager"',
            'object path "' + PREFIX + '/block_devices/vda"',
            'object path "' + PREFIX + '/block_devices/vda1"',
            'string "org.freedesktop.UDisks2.Filesystem"',
            'array of bytes "/dev/vda1"', 'array of bytes "/"',
        ]
        for fragment in fragments:
            with self.subTest(fragment=fragment):
                config = good_config()
                config['replies'][0]['stdout'] = ENUM.replace(fragment, 'absent')
                result, calls = self.run_probe(config)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(len(calls), 1)

    def test_transport_failures_at_every_stage(self):
        failures = [
            ('absent', 1, 'org.freedesktop.DBus.Error.ServiceUnknown'),
            ('denied', 1, 'org.freedesktop.DBus.Error.AccessDenied'),
            ('disconnected', 1, 'org.freedesktop.DBus.Error.Disconnected'),
            ('unknown-object', 1, 'org.freedesktop.DBus.Error.UnknownObject'),
            ('unknown-interface', 1, 'org.freedesktop.DBus.Error.UnknownInterface'),
            ('unknown-property', 1, 'org.freedesktop.DBus.Error.UnknownProperty'),
            ('timeout', 124, 'fixture timeout'),
            ('killed', 137, 'fixture killed'),
        ]
        for index in range(4):
            for name, status, diagnostic in failures:
                with self.subTest(stage=index, failure=name):
                    config = good_config()
                    # A plausible partial/full stdout on failure must never pass.
                    config['replies'][index].update(status=status, stderr=diagnostic + '\n')
                    result, calls = self.run_probe(config)
                    self.assertEqual(result.returncode, status)
                    self.assertIn(diagnostic, result.stderr)
                    self.assertEqual(len(calls), index + 1)

    def test_successful_but_empty_replies_fail(self):
        for index in range(4):
            with self.subTest(stage=index):
                config = good_config()
                config['replies'][index] = {'stdout': ''}
                result, calls = self.run_probe(config)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(len(calls), index + 1)
        config = good_config()
        config['replies'][0] = {'stdout': HEADER + '   array [\n   ]\n'}
        result, calls = self.run_probe(config)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(calls), 1)

    def test_scoped_readbacks_reject_unrelated_global_fragments(self):
        cases = [(1, device_reply('/dev/sda')), (2, device_reply('/dev/vda10')),
                 (2, device_reply('/dev/vda')), (3, mount_reply('/home')),
                 (3, mount_reply('/root')), (3, HEADER + '   variant array [\n   ]\n')]
        for index, reply in cases:
            with self.subTest(stage=index, reply=reply):
                config = good_config()
                config['replies'][index]['stdout'] = reply
                result, calls = self.run_probe(config)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('property readback rejected', result.stderr)
                self.assertEqual(len(calls), index + 1)

    def test_malformed_readbacks(self):
        valid = device_reply('/dev/vda1')
        malformed = [
            valid.replace('variant', 'string'),
            valid.replace('array of bytes', 'string'),
            valid.replace(' + \\0', ''),
            valid.replace(' + \\0', ' + \\0 trailing'),
            valid.replace('/dev/vda1', '/dev/v da1'),
            valid.replace('/dev/vda1', '/dev/vda1/'),
            valid.replace('method return ', 'error '),
            valid.replace(HEADER, ''),
            valid + '   string "extra argument"\n',
            valid + '\x00', valid + '\u00ff',
            valid + 'x' * (MODULE.MAX_REPLY + 1),
            HEADER + '   variant array of bytes [ 2f 64 65 76 2f 76 64 61 31 00 ]\n',
        ]
        for reply in malformed:
            with self.subTest(reply=reply[:140]):
                config = good_config()
                config['replies'][2]['stdout'] = reply
                result, calls = self.run_probe(config)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('property readback rejected', result.stderr)
                self.assertEqual(len(calls), 3)

    def test_mount_array_shape_and_injection_fail_closed(self):
        malformed = [
            device_reply('/'),  # ay is not aay
            mount_reply().replace('array of bytes', 'string'),
            mount_reply().replace(' + \\0', ''),
            mount_reply().replace('      ]', ''),
            mount_reply().replace('      ]', ',         ' + byte_string('/home') + '\n      ]'),
            mount_reply().replace('"/"', '"/home\n         ' + byte_string('/') + '"'),
            mount_reply() + '   variant ' + byte_string('/') + '\n',
        ]
        for reply in malformed:
            with self.subTest(reply=reply):
                config = good_config()
                config['replies'][3]['stdout'] = reply
                result, calls = self.run_probe(config)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('property readback rejected', result.stderr)
                self.assertEqual(len(calls), 4)

    def test_printer_indentation_and_line_endings(self):
        config = good_config()
        for response in config['replies'][1:]:
            response['stdout'] = response['stdout'].replace('   ', '\t').replace('\n', '\r\n')
        result, calls = self.run_probe(config)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(calls), 4)

    def test_helper_rejects_unknown_contract_and_io_errors(self):
        for prop, identity in [('Size', '/'), ('Device', '/dev/sda')]:
            with self.subTest(property=prop, identity=identity):
                with self.assertRaises(ValueError):
                    MODULE.check_reply(prop, identity, device_reply(identity))
        result = subprocess.run([sys.executable, str(CHECK), 'Device', '/dev/vda',
                                 '/nonexistent/udisks-test/reply'], text=True, capture_output=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn('property readback rejected', result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
