#!/usr/bin/env python3
"""No library build/network: exercise the real verified-cache implementation."""
import hashlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('cache', ROOT / 'userland/ports/glib-zlib-cache.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
GOOD = b'exact pinned archive bytes'
DIGEST = hashlib.sha256(GOOD).hexdigest()


class CacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.paths = [self.root / 'downloads/a.gz', self.root / 'packagecache/a.gz']
        self.calls = []

    def fetch(self, responses):
        def get(url, timeout):
            self.calls.append(url)
            self.assertEqual(timeout, 30)
            value = responses[url]
            if isinstance(value, Exception):
                raise value
            return io.BytesIO(value)
        return get

    def run_archive(self, responses):
        return m.archive(['mirror', 'upstream'], DIGEST, self.paths, self.fetch(responses))

    def test_verified_mirror(self):
        self.assertEqual(self.run_archive({'mirror': GOOD}), GOOD)
        self.assertEqual(self.calls, ['mirror'])
        for p in self.paths:
            self.assertEqual(p.read_bytes(), GOOD)

    def test_bad_hash_falls_back_without_installing_it(self):
        self.run_archive({'mirror': b'wrong but nonempty', 'upstream': GOOD})
        self.assertEqual(self.calls, ['mirror', 'upstream'])
        self.assertEqual(self.paths[0].read_bytes(), GOOD)

    def test_network_failure_falls_back(self):
        self.run_archive({'mirror': OSError('download failed'), 'upstream': GOOD})
        self.assertEqual(self.calls, ['mirror', 'upstream'])

    def test_all_bad_fail_closed_without_cache_files(self):
        with self.assertRaisesRegex(ValueError, 'no verified'):
            self.run_archive({'mirror': b'bad', 'upstream': b'also bad'})
        self.assertTrue(all(not p.exists() for p in self.paths))

    def test_valid_packagecache_repairs_poisoned_downloads_offline(self):
        for path, data in zip(self.paths, [b'poison', GOOD]):
            path.parent.mkdir(parents=True)
            path.write_bytes(data)
        self.run_archive({})
        self.assertEqual(self.calls, [])
        self.assertEqual(self.paths[0].read_bytes(), GOOD)

    def test_downloads_replay_fills_missing_packagecache(self):
        self.paths[0].parent.mkdir(parents=True)
        self.paths[0].write_bytes(GOOD)
        self.run_archive({})
        self.assertEqual(self.calls, [])
        self.assertEqual(self.paths[1].read_bytes(), GOOD)
        self.assertEqual(list(self.root.rglob('a.gz.*')), [])

    def test_size_bound(self):
        old = m.MAX_BYTES
        m.MAX_BYTES = len(GOOD)
        self.addCleanup(setattr, m, 'MAX_BYTES', old)
        self.run_archive({'mirror': GOOD + b'!', 'upstream': GOOD})
        self.assertEqual(self.calls, ['mirror', 'upstream'])

    def test_oversized_cache_is_replaced_not_reused(self):
        self.paths[0].parent.mkdir(parents=True)
        old = m.MAX_BYTES
        m.MAX_BYTES = len(GOOD)
        self.addCleanup(setattr, m, 'MAX_BYTES', old)
        self.paths[0].write_bytes(GOOD + b'poison')
        self.run_archive({'mirror': GOOD})
        self.assertEqual(self.calls, ['mirror'])
        self.assertEqual(self.paths[0].read_bytes(), GOOD)

    def test_valid_wrap_uses_exact_pinned_mirrors_and_cache_paths(self):
        from unittest.mock import patch
        source = self.root / 'glib'
        (source / 'subprojects').mkdir(parents=True)
        (source / 'subprojects/zlib.wrap').write_text('[wrap-file]\n' + '\n'.join(
            f'{k} = {v}' for k, v in {
                'directory': 'zlib-1.2.11', 'source_url': m.UPSTREAM,
                'source_filename': m.NAME, 'source_hash': m.SHA256}.items()))
        with patch.object(m, 'archive') as wrapped:
            m.prepare(source, self.root / 'dl')
            wrapped.assert_called_once_with([m.MIRROR, m.UPSTREAM], m.SHA256,
                [self.root / 'dl' / m.NAME, source / 'subprojects/packagecache' / m.NAME],
                m.urllib.request.urlopen)

    def test_changed_wrap_fails_before_network(self):
        source = self.root / 'glib'
        (source / 'subprojects').mkdir(parents=True)
        wrap = '[wrap-file]\n' + '\n'.join(f'{k} = {v}' for k, v in {
            'directory': 'zlib-1.2.11', 'source_url': m.UPSTREAM,
            'source_filename': m.NAME, 'source_hash': '0' * 64}.items())
        (source / 'subprojects/zlib.wrap').write_text(wrap)
        with self.assertRaisesRegex(ValueError, 'wrap changed'):
            m.prepare(source, self.root / 'dl', self.fetch({}))
        self.assertEqual(self.calls, [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
