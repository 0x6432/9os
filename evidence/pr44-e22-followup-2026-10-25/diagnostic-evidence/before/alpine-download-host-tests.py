#!/usr/bin/env python3
"""Native mocked downloader controls; no network, APK install, or guest proof."""
import hashlib
import importlib.util
import io
from pathlib import Path
import tempfile
import traceback
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
        self.assertEqual(getattr(error, '__notes__', []), ['Download failed: ' + URL])
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
        self.assertEqual(getattr(error, '__notes__', []), ['Download failed: ' + URL])

    def test_checksum_failure_still_fails_and_preserves_cache(self):
        self.path.write_bytes(self.old)
        with mock.patch.object(b.urllib.request, 'urlopen', side_effect=lambda *a, **k: io.BytesIO(b'wrong')) as opened, \
                mock.patch.object(b.time, 'sleep'):
            with self.assertRaisesRegex(ValueError, 'checksum mismatch') as caught:
                b.download(URL, self.path, self.sha)
        self.assertEqual(opened.call_count, 3)
        self.assertEqual(getattr(caught.exception, '__notes__', []), ['Download failed: ' + URL])
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
