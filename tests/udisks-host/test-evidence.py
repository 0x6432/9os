#!/usr/bin/env python3
"""Readback evidence controls. Command mocks only, never guest/service evidence."""
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


OLD = load('readbacks_for_evidence', HERE / 'test-readbacks.py')
EV = load('readback_evidence', OLD.EVIDENCE)
DEC = load('readback_decoder', HERE / 'decode-readback-evidence.py')
MARKER = 'alpine-udisks: real disk/partition/root filesystem enumerated uid=1000\n'


def records(output):
    return [record for line in output.splitlines()
            if (record := DEC.parse_line(line)) is not None]


class EvidenceTests(unittest.TestCase):
    def run_probe(self, config, **kwargs):
        result, calls = OLD.ReadbackTests().run_probe(config, **kwargs)
        # Deliberately the exact unchanged qemu-test.py predicate, not a looser
        # word-boundary-after-FAIL or a special diagnostic exemption.
        self.assertIsNone(re.search(rb'\bFAIL', result.stdout.encode()))
        self.assertLessEqual(len(result.stdout.removesuffix(MARKER).encode()), 3 * (3072 + 512))
        return result, calls

    def test_actual_byte_roundtrip_and_original_calls(self):
        config = OLD.good_config()
        result, calls = self.run_probe(config)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, OLD.CALLS)
        actual = records(result.stdout)
        self.assertEqual(len(actual), 3)
        for call, (fields, data) in enumerate(actual, 1):
            self.assertEqual(fields['call'], str(call))
            self.assertEqual(data, config['replies'][call]['stdout'].encode())
            self.assertEqual((fields['command_status'], fields['checker_status'], fields['typed']),
                             ('0', '0', 'passed'))
            self.assertEqual((fields['complete'], fields['truncated'], fields['unknown']), ('1', '0', '0'))

    def test_failed_command_partial_or_empty_remains_unknown_and_original_status(self):
        for stage in range(1, 4):
            for status in (1, 124, 137):
                for text in ('', 'partial FAIL\nerror bytes\n'):
                    with self.subTest(stage=stage, status=status, text=text):
                        config = OLD.good_config()
                        config['replies'][stage] = {'status': status, 'stdout': text,
                                                    'stderr': 'original_dbus_diagnostic\n'}
                        result, calls = self.run_probe(config)
                        self.assertEqual(result.returncode, status)
                        self.assertEqual(result.stderr, 'original_dbus_diagnostic\n')
                        self.assertEqual(len(calls), stage + 1)
                        fields, data = records(result.stdout)[-1]
                        self.assertEqual(fields['command_status'], str(status))
                        self.assertEqual(fields['checker_status'], 'unknown')
                        self.assertEqual(fields['typed'], 'unknown')
                        self.assertEqual(fields['complete'], 'unknown')
                        self.assertEqual(fields['unknown'], '1')
                        self.assertIsNone(data)
                        self.assertEqual(fields['captured_bytes'], str(len(text)))
                        self.assertEqual(fields['data_hex'], text.encode().hex() if text else 'empty')

    def test_original_stderr_not_recoded_or_hidden(self):
        config = OLD.good_config()
        config['replies'][1].update(status=1, stderr='original stderr FAIL remains authoritative\n')
        result, _ = self.run_probe(config)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, 'original stderr FAIL remains authoritative\n')

    def test_checker_rejection_still_failed_but_complete_raw_bytes_recoverable(self):
        config = OLD.good_config()
        payload = OLD.device_reply('/dev/FAIL')
        config['replies'][2]['stdout'] = payload
        result, calls = self.run_probe(config)
        self.assertEqual(result.returncode, 1)
        self.assertIn('property readback rejected', result.stderr)
        self.assertEqual(len(calls), 3)
        fields, data = records(result.stdout)[-1]
        self.assertEqual(fields['typed'], 'failed')
        self.assertEqual(fields['checker_status'], '1')
        self.assertEqual(data, payload.encode())

    def test_empty_successful_file_is_not_complete_reply(self):
        config = OLD.good_config()
        config['replies'][1]['stdout'] = ''
        result, _ = self.run_probe(config)
        self.assertEqual(result.returncode, 1)
        fields, data = records(result.stdout)[0]
        self.assertEqual((fields['capture'], fields['complete'], fields['truncated'], fields['unknown']),
                         ('empty', '0', '0', '1'))
        self.assertEqual(fields['typed'], 'failed')
        self.assertEqual(fields['captured_bytes'], '0')  # observed empty file, not invented reply
        self.assertEqual(fields['data_hex'], 'empty')
        self.assertIsNone(data)

    def test_capture_boundary_does_not_add_an_acceptance_limit(self):
        for size in (1024, 1025, OLD.MODULE.MAX_REPLY, OLD.MODULE.MAX_REPLY + 1):
            with self.subTest(size=size):
                config = OLD.good_config()
                reply = config['replies'][1]['stdout']
                config['replies'][1]['stdout'] = reply + ' ' * (size - len(reply))
                result, calls = self.run_probe(config)
                self.assertEqual(result.returncode, 0 if size <= OLD.MODULE.MAX_REPLY else 1)
                fields, data = records(result.stdout)[0]
                self.assertEqual(fields['captured_bytes'], str(min(size, 1024)))
                self.assertEqual(bytes.fromhex(fields['data_hex']), config['replies'][1]['stdout'].encode()[:1024])
                self.assertEqual(fields['complete'], '1' if size == 1024 else '0')
                self.assertEqual(fields['truncated'], '0' if size == 1024 else '1')
                self.assertEqual(fields['unknown'], '0' if size == 1024 else '1')
                self.assertEqual(data is None, size != 1024)

    def test_binary_nul_newline_sentinel_payload_is_only_lowercase_hex(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'reply'
            payload = b'\x00\xffFAIL\nFAILED\nFAILURE\n' + bytes(range(256)) * 8
            path.write_bytes(payload)
            line = EV.record('3', '0', '1', path)
            self.assertLessEqual(len(line.encode()), 3072)
            self.assertIsNone(re.search(rb'\bFAIL', line.encode()))
            fields, data = DEC.parse_line(line)
            self.assertEqual(bytes.fromhex(fields['data_hex']), payload[:1024])
            self.assertEqual(fields['typed'], 'failed')
            self.assertIsNone(data)

    def test_missing_file_never_fabricates_zero_bytes(self):
        with tempfile.TemporaryDirectory() as temp:
            line = EV.record('1', '0', '0', Path(temp) / 'absent')
        fields, data = DEC.parse_line(line)
        self.assertEqual(fields['recording'], 'error')
        self.assertEqual(fields['typed'], 'passed')  # retained checker result, not evidence claim
        for key in ('capture', 'complete', 'truncated', 'captured_bytes', 'data_hex'):
            self.assertEqual(fields[key], 'unknown')
        self.assertEqual(fields['unknown'], '1')
        self.assertIsNone(data)

    def test_capture_io_failure_after_checker_cannot_veto_success(self):
        with tempfile.TemporaryDirectory() as temp:
            wrapper = Path(temp) / 'remove-before-recording.py'
            wrapper.write_text('import os,runpy,sys\nos.unlink(sys.argv[4])\nrunpy.run_path(' +
                               repr(str(OLD.EVIDENCE)) + ',run_name="__main__")\n')
            result, calls = self.run_probe(OLD.good_config(), recorder=wrapper)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(calls, OLD.CALLS)
            actual = records(result.stdout)
            self.assertEqual(len(actual), 3)
            self.assertTrue(all(f['recording'] == 'error' and data is None for f, data in actual))

    def test_recorder_exit_error_does_not_mask_any_original_outcome(self):
        with tempfile.TemporaryDirectory() as temp:
            recorder = Path(temp) / 'broken.py'
            recorder.write_text('raise SystemExit(9)\n')
            for command_status, checker_failure in ((0, False), (1, False), (124, False), (137, False), (0, True)):
                with self.subTest(command_status=command_status, checker_failure=checker_failure):
                    config = OLD.good_config()
                    if command_status:
                        config['replies'][1].update(status=command_status, stderr='dbus_problem\n')
                    if checker_failure:
                        config['replies'][1]['stdout'] = OLD.device_reply('/dev/wrong')
                    result, calls = self.run_probe(config, recorder=recorder)
                    expected = command_status or int(checker_failure)
                    self.assertEqual(result.returncode, expected)
                    self.assertEqual(len(calls), 2 if expected else 4)
                    actual = records(result.stdout)
                    self.assertTrue(actual)
                    self.assertTrue(all(f['recording'] == 'error' and data is None for f, data in actual))
                    self.assertTrue(all(f['complete'] == 'unknown' and f['captured_bytes'] == 'unknown' for f, _ in actual))

    def test_missing_recorder_explicit_fallback_preserves_success(self):
        result, calls = self.run_probe(OLD.good_config(), recorder=HERE / 'absent-recorder.py')
        self.assertEqual(result.returncode, 0)
        self.assertEqual(calls, OLD.CALLS)
        self.assertEqual(len(records(result.stdout)), 3)
        self.assertTrue(all(f['recording'] == 'error' for f, _ in records(result.stdout)))

    def test_partial_record_then_error_remains_safe_and_hard_bounded(self):
        with tempfile.TemporaryDirectory() as temp:
            recorder = Path(temp) / 'partial.py'
            recorder.write_text('import sys\nsys.stdout.write("a"*3072)\nsys.stdout.flush()\nraise SystemExit(9)\n')
            result, calls = self.run_probe(OLD.good_config(), recorder=recorder)
            self.assertEqual(result.returncode, 0)
            self.assertEqual(len(records(result.stdout)), 3)  # leading newline in safe fallback
            self.assertEqual(calls, OLD.CALLS)
            for line in result.stdout.splitlines():
                if line.startswith('udisks_readback_v1 '):
                    self.assertLessEqual(len(line.encode()) + 2, 512)

    def test_reader_requests_at_most_cap_plus_one(self):
        class Reader(io.BytesIO):
            def read(inner, size=-1):
                self.assertEqual(size, 1025)
                return super().read(size)
        with patch('builtins.open', return_value=Reader(b'x' * 2000)):
            fields, data = DEC.parse_line(EV.record('2', '1', 'unknown', 'fixture'))
        self.assertEqual(fields['captured_bytes'], '1024')
        self.assertEqual(fields['truncated'], '1')
        self.assertEqual(fields['complete'], 'unknown')
        self.assertIsNone(data)

    def test_invalid_metadata_never_echoed(self):
        for args in [('FAIL', '0', '0'), ('1', 'FAIL', '0'), ('1', '256', '0'),
                     ('1', '0', 'FAIL'), ('1', '1', '0')]:
            with self.subTest(args=args):
                result = subprocess.run([sys.executable, str(OLD.EVIDENCE), *args, '/absent'],
                                        capture_output=True, text=True, timeout=3)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, '')
                self.assertEqual(result.stderr, 'udisks_readback_recording_error\n')

    def test_offline_decoder_roundtrip_from_timestamped_artifact_style(self):
        result, _ = self.run_probe(OLD.good_config())
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); log = root / 'job.log'; out = root / 'decoded'
            log.write_text('unrelated unicode noise: \u2014\n' + ''.join('2026-10-19T00:00:00.1234567Z ' + l + '\n' for l in result.stdout.splitlines()))
            decoded = subprocess.run([sys.executable, str(HERE / 'decode-readback-evidence.py'),
                                      str(log), '--output-dir', str(out)], capture_output=True, text=True, timeout=3)
            self.assertEqual(decoded.returncode, 0, decoded.stderr)
            self.assertEqual(len(list(out.iterdir())), 3)
            for call in range(1, 4):
                self.assertEqual((out / f'{call:04d}-call-{call}.reply').read_bytes(),
                                 OLD.good_config()['replies'][call]['stdout'].encode())

    def test_decoder_rejects_metadata_corruption_and_never_promotes_unknown(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'reply'; path.write_bytes(b'payload')
            line = EV.record('1', '0', '0', path)
            for corrupted in (line.replace('call=1', 'call=3'), line.replace('data_hex=7061796c6f6164', 'data_hex=FAIL'),
                              line.replace('captured_bytes=7', 'captured_bytes=0'), line.replace('command_status=0', 'command_status=1'),
                              line.replace('unknown=0', 'unknown=1'), line.replace('recording=ok', 'recording=error')):
                with self.subTest(corrupted=corrupted):
                    with self.assertRaises(ValueError):
                        DEC.parse_line(corrupted)
            unknown = EV.record('1', '1', 'unknown', path)
            self.assertIsNone(DEC.parse_line(unknown)[1])

    def test_checker_nonzero_status_is_preserved_exactly(self):
        with tempfile.TemporaryDirectory() as temp:
            checker = Path(temp) / 'checker-error.py'
            for status in (2, 3, 127):
                with self.subTest(status=status):
                    checker.write_text('import sys\nsys.stderr.write("original_checker_problem\\n")\nraise SystemExit(' + str(status) + ')\n')
                    source = OLD.PROBE.read_text().replace('/usr/share/9os/udisks-property-check.py', str(checker))
                    result, calls = self.run_probe(OLD.good_config(), probe_text=source)
                    self.assertEqual(result.returncode, status)
                    self.assertEqual(result.stderr, 'original_checker_problem\n')
                    self.assertEqual(len(calls), 2)
                    fields, data = records(result.stdout)[0]
                    self.assertEqual(fields['command_status'], '0')
                    self.assertEqual(fields['checker_status'], str(status))
                    self.assertEqual(fields['typed'], 'failed')

    def test_output_error_is_reported_without_echoing_exception_text(self):
        class Broken:
            def write(self, text):
                raise OSError('FAIL arbitrary output exception')
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'reply'; path.write_bytes(b'FAIL payload')
            stderr = io.StringIO()
            with patch.object(sys, 'argv', [str(OLD.EVIDENCE), '1', '0', '1', str(path)]), \
                 patch.object(sys, 'stdout', Broken()), patch.object(sys, 'stderr', stderr):
                status = EV.main()
        self.assertEqual(status, 1)
        self.assertEqual(stderr.getvalue(), 'udisks_readback_recording_error\n')

    def test_decoder_never_creates_reply_files_for_missing_or_failed_capture(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); path = root / 'empty'; path.write_bytes(b'')
            log = root / 'log'; out = root / 'decoded'
            log.write_text(EV.record('1', '1', 'unknown', path) +
                           EV.record('2', '0', '0', root / 'absent'))
            result = subprocess.run([sys.executable, str(HERE / 'decode-readback-evidence.py'),
                                     str(log), '--output-dir', str(out)], capture_output=True, text=True, timeout=3)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(list(out.iterdir()), [])
            log.write_text(MARKER)
            absent = root / 'not-created'
            result = subprocess.run([sys.executable, str(HERE / 'decode-readback-evidence.py'),
                                     str(log), '--output-dir', str(absent)], capture_output=True, text=True, timeout=3)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(absent.exists())

    def test_production_checker_bytecode_source_and_original_query_bounds_unchanged(self):
        import hashlib
        baseline = (HERE / 'evidence/original-gap-native-baseline.log').read_text()
        expected = re.search(r'checker sha256=([0-9a-f]{64})', baseline)[1]
        self.assertEqual(hashlib.sha256(OLD.CHECK.read_bytes()).hexdigest(), expected)
        text = OLD.PROBE.read_text()
        self.assertEqual(text.count('org.freedesktop.DBus.ObjectManager.GetManagedObjects'), 1)
        self.assertEqual(text.count('org.freedesktop.DBus.Properties.Get'), 2)  # method plus fixed fallback metadata
        self.assertIn('timeout -s KILL 40 dbus-send --system --print-reply --reply-timeout=30000', text)
        self.assertIn('timeout -s KILL 6 dbus-send --system --print-reply --reply-timeout=4000', text)
        self.assertIn('[ "$(id -u)" = 1000 ]', text)


if __name__ == '__main__':
    unittest.main(verbosity=2)
