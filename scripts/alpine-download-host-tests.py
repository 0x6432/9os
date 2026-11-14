#!/usr/bin/env python3
"""Native mocked downloader controls; no network, APK install, or guest proof."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import traceback
import types
import unittest
from unittest import mock
import urllib.error

TOP = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('alpine_download', TOP / 'userland/alpine/build.py')
b = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(b)
URL = b.BASE + '/' + b.BRANCH + '/main/x86_64/pcre2-10.46-r1.apk'


class AlpineDownload(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / 'pcre2.apk'
        self.part = self.path.with_suffix('.apk.part')
        self.old = b'previous-cache-preserved'
        self.payload = b'verified-test-payload'
        self.sha = hashlib.sha256(self.payload).hexdigest()

    def terminal_records(self, error):
        notes = getattr(error, '__notes__', [])
        self.assertEqual(len(notes), 2, 'expected URL note then one attempt summary')
        self.assertEqual(notes[0], 'Download failed: ' + URL)
        self.assertTrue(notes[1].startswith('Download attempts: '))
        self.assertLessEqual(len(notes[1].encode('utf-8')), 1024)
        records = json.loads(notes[1][len('Download attempts: '):])
        self.assertEqual(len(records), 3)
        self.assertEqual([r['attempt'] for r in records], [1, 2, 3])
        for r in records:
            self.assertEqual(set(r), {'attempt', 'utc_start', 'elapsed_s', 'class', 'http_status'})
            if r['utc_start'] != 'unknown':
                self.assertRegex(r['utc_start'], r'^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$')
            if r['elapsed_s'] != 'unknown':
                self.assertGreaterEqual(r['elapsed_s'], 0)
            self.assertIn(r['class'], ['HTTPError', 'TimeoutError', 'URLError', 'OSError', 'other'])
            self.assertTrue(r['http_status'] == 'unknown' or
                            type(r['http_status']) is int and 100 <= r['http_status'] <= 599)
        return records

    def mixed_contract(self, module):
        self.path.write_bytes(self.old)
        self.part.write_bytes(b'stale-partial')
        errors = [urllib.error.HTTPError(URL, 404, 'DO_NOT_LOG_MESSAGE',
                                        {'X-Private': 'DO_NOT_LOG_HEADER'}, None),
                  TimeoutError('DO_NOT_LOG_TIMEOUT'),
                  urllib.error.HTTPError(URL, 503, 'DO_NOT_LOG_FINAL', {}, None)]
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(module.urllib.request, 'urlopen', side_effect=errors) as opened, \
                mock.patch.object(module.time, 'sleep') as sleep, \
                mock.patch.object(module.time, 'time', side_effect=[1700000000, 1700000001, 1700000003]) as wall, \
                mock.patch.object(module.time, 'monotonic', side_effect=[100, 100.125, 101.125, 101.625, 103.625, 104.625]) as clock, \
                contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            with self.assertRaises(urllib.error.HTTPError) as caught:
                module.download(URL, self.path, self.sha)
        self.assertIs(caught.exception, errors[-1])
        self.assertEqual(opened.call_args_list, [mock.call(URL, timeout=60)] * 3)
        self.assertEqual(sleep.call_args_list, [mock.call(1), mock.call(2)])
        self.assertEqual(wall.call_count, 3)
        self.assertEqual(clock.call_count, 6)
        records = self.terminal_records(caught.exception)
        self.assertEqual(records, [
            {'attempt': 1, 'utc_start': '2023-11-14T22:13:20Z', 'elapsed_s': 0.125,
             'class': 'HTTPError', 'http_status': 404},
            {'attempt': 2, 'utc_start': '2023-11-14T22:13:21Z', 'elapsed_s': 0.5,
             'class': 'TimeoutError', 'http_status': 'unknown'},
            {'attempt': 3, 'utc_start': '2023-11-14T22:13:23Z', 'elapsed_s': 1.0,
             'class': 'HTTPError', 'http_status': 503}])
        for error in errors[:-1]:
            self.assertEqual(getattr(error, '__notes__', []), [])
        self.assertNotIn('DO_NOT_LOG', caught.exception.__notes__[1])
        self.assertEqual(stdout.getvalue(), '')
        self.assertEqual(stderr.getvalue(), '')
        self.assertFalse(self.part.exists())
        self.assertEqual(self.path.read_bytes(), self.old)

    def test_semantic_guard_mutants_cannot_mask_errors_or_corrupt_summary(self):
        source = (TOP / 'userland/alpine/build.py').read_text()
        self.clock_failure_contract(b, 'time')
        self.summary_fault_contract(b, 'serialization')
        mutants = [
            ('start-clock-unguarded', "except Exception:\n            utc_start = 'unknown'",
             "except ValueError:\n            utc_start = 'unknown'", 'clock-start'),
            ('end-clock-unguarded', "except Exception:\n                elapsed = 'unknown'",
             "except ValueError:\n                elapsed = 'unknown'", 'clock-end'),
            ('serialization-unguarded',
             'except Exception:\n                    pass  # Omit only the new diagnostic on formatting/attachment failure.',
             'except ValueError:\n                    pass  # Omit only the new diagnostic on formatting/attachment failure.', 'serialization'),
            ('oversize-note-allowed', "if len(summary.encode('utf-8')) <= 1024:",
             'if True:', 'oversize'),
            ('nonfinite-json-allowed', 'allow_nan=False', 'allow_nan=True', 'nonfinite')]
        for name, old, new, fault in mutants:
            with self.subTest(mutant=name):
                self.assertEqual(source.count(old), 1, 'mutation anchor changed: ' + name)
                module = types.ModuleType('guard_mutant_' + name.replace('-', '_'))
                module.__file__ = str(TOP / 'userland/alpine/build.py')
                exec(compile(source.replace(old, new, 1), '<' + name + '>', 'exec'), module.__dict__)
                with self.assertRaises(AssertionError, msg='semantic guard mutant survived: ' + name):
                    if fault in ['clock-start', 'clock-end']:
                        self.clock_failure_contract(module, 'time' if fault == 'clock-start' else 'elapsed-end')
                    else:
                        self.summary_fault_contract(module, fault)
                print('SEMANTIC GUARD MUTANT REJECTED:', name)

    def summary_fault_contract(self, module, fault):
        class AttachmentError(urllib.error.HTTPError):
            def add_note(self, note):
                if note.startswith('Download attempts: '):
                    raise RuntimeError('DO_NOT_LOG_ATTACHMENT')
                return super().add_note(note)
        cls = AttachmentError if fault == 'attachment' else urllib.error.HTTPError
        error = cls(URL, 503, 'DO_NOT_LOG_NETWORK', {}, None)
        self.path.write_bytes(self.old)
        self.part.write_bytes(b'stale-partial')
        stdout, stderr = io.StringIO(), io.StringIO()
        caught = None
        oversize = json.dumps([{'padding': '\u96ea' * 400}], ensure_ascii=False)
        with contextlib.ExitStack() as stack:
            if fault == 'serialization':
                stack.enter_context(mock.patch.object(module.json, 'dumps', side_effect=RuntimeError('DO_NOT_LOG_FORMAT')))
            elif fault == 'oversize':
                stack.enter_context(mock.patch.object(module.json, 'dumps', return_value=oversize))
            elif fault == 'nonfinite':
                stack.enter_context(mock.patch.object(module.time, 'monotonic', return_value=float('nan')))
            opened = stack.enter_context(mock.patch.object(module.urllib.request, 'urlopen', side_effect=error))
            sleep = stack.enter_context(mock.patch.object(module.time, 'sleep'))
            stack.enter_context(contextlib.redirect_stdout(stdout))
            stack.enter_context(contextlib.redirect_stderr(stderr))
            try:
                module.download(URL, self.path, self.sha)
            except Exception as observed:
                caught = observed
        self.assertIs(caught, error, 'optional summary masked the final download error')
        self.assertEqual(opened.call_args_list, [mock.call(URL, timeout=60)] * 3)
        self.assertEqual(sleep.call_args_list, [mock.call(1), mock.call(2)])
        self.assertEqual(getattr(error, '__notes__', []), ['Download failed: ' + URL])
        self.assertEqual(self.path.read_bytes(), self.old)
        self.assertFalse(self.part.exists())
        self.assertEqual((stdout.getvalue(), stderr.getvalue()), ('', ''))

    def test_summary_faults_or_oversize_only_omit_optional_note(self):
        for fault in ['serialization', 'attachment', 'oversize', 'nonfinite']:
            with self.subTest(fault=fault):
                self.summary_fault_contract(b, fault)

    def test_summary_guard_does_not_swallow_base_exceptions(self):
        for stop in [SystemExit(8), KeyboardInterrupt(), BaseException('stop')]:
            error = urllib.error.HTTPError(URL, 503, 'private', {}, None)
            with self.subTest(kind=type(stop).__name__), \
                    mock.patch.object(b.urllib.request, 'urlopen', side_effect=error) as opened, \
                    mock.patch.object(b.time, 'sleep') as sleep, \
                    mock.patch.object(b.json, 'dumps', side_effect=stop):
                with self.assertRaises(type(stop)) as caught:
                    b.download(URL, self.path, self.sha)
                self.assertIs(caught.exception, stop)
                self.assertEqual(opened.call_args_list, [mock.call(URL, timeout=60)] * 3)
                self.assertEqual(sleep.call_args_list, [mock.call(1), mock.call(2)])
                self.assertEqual(error.__notes__, ['Download failed: ' + URL])
                self.assertFalse(self.part.exists())

    def clock_fault(self, module, fault):
        error = OSError('DO_NOT_LOG_CLOCK_FAULT')
        if fault == 'elapsed-end':
            return mock.patch.object(module.time, 'monotonic',
                                     side_effect=[100, error, 200, error, 300, error])
        if fault == 'elapsed-format':
            return mock.patch.object(module, 'round', side_effect=error, create=True)
        return mock.patch.object(module.time, fault, side_effect=error)

    def clock_failure_contract(self, module, fault):
        self.path.write_bytes(self.old)
        self.part.write_bytes(b'stale-partial')
        error = urllib.error.HTTPError(URL, 503, 'DO_NOT_LOG_NETWORK', {}, None)
        stdout, stderr = io.StringIO(), io.StringIO()
        caught = None
        with self.clock_fault(module, fault), \
                mock.patch.object(module.urllib.request, 'urlopen', side_effect=error) as opened, \
                mock.patch.object(module.time, 'sleep') as sleep, \
                contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            try:
                module.download(URL, self.path, self.sha)
            except Exception as observed:
                caught = observed
        self.assertIs(caught, error, 'diagnostic clock fault masked terminal network exception')
        self.assertEqual(opened.call_args_list, [mock.call(URL, timeout=60)] * 3)
        self.assertEqual(sleep.call_args_list, [mock.call(1), mock.call(2)])
        records = self.terminal_records(caught)
        key = 'elapsed_s' if fault in ['monotonic', 'elapsed-end', 'elapsed-format'] else 'utc_start'
        self.assertTrue(all(r[key] == 'unknown' for r in records))
        self.assertTrue(all(r['class'] == 'HTTPError' and r['http_status'] == 503 for r in records))
        self.assertNotIn('DO_NOT_LOG', caught.__notes__[1])
        self.assertEqual(self.path.read_bytes(), self.old)
        self.assertFalse(self.part.exists())
        self.assertEqual((stdout.getvalue(), stderr.getvalue()), ('', ''))

    def test_clock_faults_cannot_mask_terminal_network_exception(self):
        for fault in ['time', 'gmtime', 'strftime', 'monotonic', 'elapsed-end', 'elapsed-format']:
            with self.subTest(fault=fault):
                self.clock_failure_contract(b, fault)

    def test_clock_faults_cannot_break_successful_download(self):
        for fault in ['time', 'gmtime', 'strftime', 'monotonic']:
            with self.subTest(fault=fault):
                self.path.unlink(missing_ok=True)
                stdout, stderr = io.StringIO(), io.StringIO()
                caught = None
                with self.clock_fault(b, fault), \
                        mock.patch.object(b.urllib.request, 'urlopen', return_value=io.BytesIO(self.payload)) as opened, \
                        mock.patch.object(b.time, 'sleep') as sleep, \
                        mock.patch.object(b.json, 'dumps') as serialized, \
                        contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                    try:
                        b.download(URL, self.path, self.sha)
                    except Exception as observed:
                        caught = observed
                self.assertIsNone(caught, 'diagnostic clock fault blocked successful download')
                self.assertEqual(opened.call_args_list, [mock.call(URL, timeout=60)])
                sleep.assert_not_called()
                self.assertEqual(self.path.read_bytes(), self.payload)
                self.assertFalse(self.part.exists())
                self.assertEqual((stdout.getvalue(), stderr.getvalue()), ('', ''))

    def test_clock_guard_does_not_swallow_base_exceptions(self):
        for error in [SystemExit(7), KeyboardInterrupt(), BaseException('stop')]:
            with self.subTest(kind=type(error).__name__), \
                    mock.patch.object(b.time, 'time', side_effect=error), \
                    mock.patch.object(b.urllib.request, 'urlopen') as opened:
                with self.assertRaises(type(error)) as caught:
                    b.download(URL, self.path, self.sha)
                self.assertIs(caught.exception, error)
                opened.assert_not_called()

    def test_semantic_mutants_are_rejected_by_the_mixed_contract(self):
        source = (TOP / 'userland/alpine/build.py').read_text()
        # Positive control first; compile/initialization failures are outside the
        # assertion below and must fail, not masquerade as a killed mutant.
        self.mixed_contract(b)
        mutants = [
            ('missing-summary',
             "error.add_note(summary)",
             'pass  # remove only the attempt summary'),
            ('wrong-attempt-index', "'attempt': attempt + 1", "'attempt': attempt"),
            ('lost-http-status', "status = error.code if kind == 'HTTPError' else None", 'status = None'),
            ('wrong-elapsed', 'round(time.monotonic() - started, 3)',
             'round(time.monotonic() - started + 1, 3)'),
            ('timeout-misclassified', 'elif isinstance(error, TimeoutError):',
             'elif isinstance(error, type(None)):'),
            ('url-note-altered', "error.add_note('Download failed: ' + url)",
             "error.add_note('Download failed: ' + url + ' altered')")]
        for name, old, new in mutants:
            with self.subTest(mutant=name):
                self.assertEqual(source.count(old), 1, 'mutation anchor changed: ' + name)
                changed = source.replace(old, new, 1)
                module = types.ModuleType('download_mutant_' + name.replace('-', '_'))
                module.__file__ = str(TOP / 'userland/alpine/build.py')
                exec(compile(changed, '<' + name + '>', 'exec'), module.__dict__)
                with self.assertRaises(AssertionError, msg='semantic mutant survived: ' + name):
                    self.mixed_contract(module)
                print('SEMANTIC MUTANT REJECTED:', name)

    def test_mixed_attempt_diagnostic_contract(self):
        self.mixed_contract(b)

    def test_fixed_classes_status_range_and_output_bound(self):
        enormous = type('DO_NOT_LOG_CLASS_' + 'X' * 2000, (Exception,), {})
        cases = [(urllib.error.HTTPError(URL, 100, 'secret', {}, None), 'HTTPError', 100),
                 (urllib.error.HTTPError(URL, 599, 'secret', {}, None), 'HTTPError', 599),
                 (urllib.error.HTTPError(URL, 99, 'secret', {}, None), 'HTTPError', 'unknown'),
                 (urllib.error.HTTPError(URL, 600, 'secret', {}, None), 'HTTPError', 'unknown'),
                 (urllib.error.HTTPError(URL, '404', 'secret', {}, None), 'HTTPError', 'unknown'),
                 (urllib.error.URLError('secret'), 'URLError', 'unknown'),
                 (OSError('secret'), 'OSError', 'unknown'),
                 (enormous('DO_NOT_LOG_' * 2000), 'other', 'unknown')]
        for error, kind, status in cases:
            with self.subTest(kind=kind, status=status), \
                    mock.patch.object(b.urllib.request, 'urlopen', side_effect=error) as opened, \
                    mock.patch.object(b.time, 'sleep'), \
                    self.assertRaises(type(error)) as caught:
                b.download(URL, self.path, self.sha)
            self.assertIs(caught.exception, error)
            self.assertEqual(opened.call_count, 3)
            records = self.terminal_records(error)
            self.assertTrue(all(r['class'] == kind and r['http_status'] == status for r in records))
            self.assertNotIn('DO_NOT_LOG', error.__notes__[1])

    def test_elapsed_covers_request_stream_and_hash_not_backoff(self):
        clock = [1000.0]
        class Timed(io.BytesIO):
            def read(self, size=-1):
                chunk = super().read(size)
                if chunk:
                    clock[0] += 2
                return chunk
        def opened(*args, **kwargs):
            clock[0] += 1
            return Timed(b'wrong')
        def digest(path):
            clock[0] += 3
            return 'not-the-reviewed-sha'
        def sleep(seconds):
            clock[0] += seconds
        with mock.patch.object(b.urllib.request, 'urlopen', side_effect=opened), \
                mock.patch.object(b, 'digest', side_effect=digest), \
                mock.patch.object(b.time, 'time', return_value=1700000000), \
                mock.patch.object(b.time, 'monotonic', side_effect=lambda: clock[0]), \
                mock.patch.object(b.time, 'sleep', side_effect=sleep) as slept:
            with self.assertRaises(ValueError) as caught:
                b.download(URL, self.path, self.sha)
        self.assertEqual([r['elapsed_s'] for r in self.terminal_records(caught.exception)], [6, 6, 6])
        self.assertEqual(slept.call_args_list, [mock.call(1), mock.call(2)])
        self.assertFalse(self.path.exists())
        self.assertFalse(self.part.exists())

    def test_recovery_after_two_failures_is_quiet_and_leaves_prior_errors_unchanged(self):
        errors = [urllib.error.HTTPError(URL, 404, 'private', {}, None), TimeoutError('private')]
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(b.urllib.request, 'urlopen', side_effect=errors + [io.BytesIO(self.payload)]) as opened, \
                mock.patch.object(b.time, 'sleep') as sleep, \
                mock.patch.object(b.json, 'dumps', side_effect=RuntimeError('must not serialize')) as serialized, \
                contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            b.download(URL, self.path, self.sha)
        self.assertEqual(opened.call_args_list, [mock.call(URL, timeout=60)] * 3)
        self.assertEqual(sleep.call_args_list, [mock.call(1), mock.call(2)])
        self.assertEqual(self.path.read_bytes(), self.payload)
        self.assertFalse(self.part.exists())
        self.assertEqual([getattr(e, '__notes__', []) for e in errors], [[], []])
        serialized.assert_not_called()
        self.assertEqual((stdout.getvalue(), stderr.getvalue()), ('', ''))

    def test_cache_and_offline_paths_are_quiet_without_clocks_or_network(self):
        for cached in [True, False]:
            with self.subTest(cached=cached):
                if cached:
                    self.path.write_bytes(self.payload)
                else:
                    self.path.unlink(missing_ok=True)
                stdout, stderr = io.StringIO(), io.StringIO()
                with mock.patch.object(b.urllib.request, 'urlopen') as opened, \
                        mock.patch.object(b.time, 'time') as wall, \
                        mock.patch.object(b.time, 'monotonic') as clock, \
                        mock.patch.object(b.time, 'sleep') as sleep, \
                        mock.patch.object(b.json, 'dumps') as serialized, \
                        contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                    if cached:
                        b.download(URL, self.path, self.sha, offline=True)
                    else:
                        with self.assertRaises(ValueError) as caught:
                            b.download(URL, self.path, self.sha, offline=True)
                        self.assertEqual(getattr(caught.exception, '__notes__', []), [])
                for control in [opened, wall, clock, sleep, serialized]:
                    control.assert_not_called()
                self.assertEqual((stdout.getvalue(), stderr.getvalue()), ('', ''))

    def test_final_http404_keeps_exception_and_reports_url(self):
        self.path.write_bytes(self.old)
        self.part.write_bytes(b'stale-partial')
        error = urllib.error.HTTPError(URL, 404, 'Not Found', {}, None)
        with mock.patch.object(b.urllib.request, 'urlopen', side_effect=error) as opened, \
                mock.patch.object(b.time, 'sleep') as sleep:
            with self.assertRaises(urllib.error.HTTPError) as caught:
                b.download(URL, self.path, self.sha)
        self.assertIs(caught.exception, error)
        self.assertEqual(error.code, 404)
        self.assertEqual(opened.call_args_list, [mock.call(URL, timeout=60)] * 3)
        self.assertEqual(sleep.call_args_list, [mock.call(1), mock.call(2)])
        self.terminal_records(error)
        self.assertIn('Download failed: ' + URL, ''.join(traceback.format_exception(error)))
        self.assertFalse(self.part.exists())
        self.assertEqual(self.path.read_bytes(), self.old)

    def test_partial_read_failure_is_cleaned_without_replacing_cache(self):
        self.path.write_bytes(self.old)
        error = OSError('interrupted read')
        class Broken(io.BytesIO):
            def read(self, size=-1):
                if self.tell():
                    raise error
                return super().read(2)
        with mock.patch.object(b.urllib.request, 'urlopen', side_effect=lambda *a, **k: Broken(b'partial')) as opened, \
                mock.patch.object(b.time, 'sleep') as sleep:
            with self.assertRaises(OSError) as caught:
                b.download(URL, self.path, self.sha)
        self.assertIs(caught.exception, error)
        self.assertEqual(opened.call_count, 3)
        self.assertEqual(sleep.call_args_list, [mock.call(1), mock.call(2)])
        self.assertFalse(self.part.exists())
        self.assertEqual(self.path.read_bytes(), self.old)
        self.terminal_records(error)

    def test_checksum_failure_still_fails_and_preserves_cache(self):
        self.path.write_bytes(self.old)
        with mock.patch.object(b.urllib.request, 'urlopen', side_effect=lambda *a, **k: io.BytesIO(b'wrong')) as opened, \
                mock.patch.object(b.time, 'sleep'):
            with self.assertRaisesRegex(ValueError, 'checksum mismatch') as caught:
                b.download(URL, self.path, self.sha)
        self.assertEqual(opened.call_count, 3)
        self.terminal_records(caught.exception)
        self.assertFalse(self.part.exists())
        self.assertEqual(self.path.read_bytes(), self.old)

    def test_recovery_after_one_failure_has_no_final_failure_note(self):
        self.path.write_bytes(self.old)
        error = urllib.error.HTTPError(URL, 404, 'Not Found', {}, None)
        with mock.patch.object(b.urllib.request, 'urlopen', side_effect=[error, io.BytesIO(self.payload)]) as opened, \
                mock.patch.object(b.time, 'sleep') as sleep:
            b.download(URL, self.path, self.sha)
        self.assertEqual(opened.call_count, 2)
        self.assertEqual(sleep.call_args_list, [mock.call(1)])
        self.assertEqual(self.path.read_bytes(), self.payload)
        self.assertFalse(self.part.exists())
        self.assertEqual(getattr(error, '__notes__', []), [])

    def test_matching_cache_offline_makes_no_network_request(self):
        self.path.write_bytes(self.payload)
        with mock.patch.object(b.urllib.request, 'urlopen') as opened:
            b.download(URL, self.path, self.sha, offline=True)
        opened.assert_not_called()
        self.assertEqual(self.path.read_bytes(), self.payload)

    def test_missing_offline_cache_still_fails_without_network(self):
        with mock.patch.object(b.urllib.request, 'urlopen') as opened:
            with self.assertRaisesRegex(ValueError, 'Offline cache missing or checksum mismatch'):
                b.download(URL, self.path, self.sha, offline=True)
        opened.assert_not_called()
        self.assertFalse(self.path.exists())


if __name__ == '__main__':
    unittest.main()
