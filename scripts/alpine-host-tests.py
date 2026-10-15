#!/usr/bin/env python3
"""Small negative controls for the fast lane's artifact/lock trust boundary."""
import hashlib, importlib.util, io, pathlib, tempfile, unittest
from unittest.mock import patch
p = pathlib.Path(__file__).resolve().parents[1] / 'userland/alpine/build.py'
spec = importlib.util.spec_from_file_location('alpine_builder', p)
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

class TrustTests(unittest.TestCase):
    def entry(self, **kw):
        return dict(url=m.BASE+'/v3.22/main/x86_64/musl-1.2.5-r12.apk', sha256='a'*64, **kw)
    def test_strict_locked_urls(self):
        good = self.entry(); self.assertEqual(m.validate_entries([good], 'x86_64', True), [good])
        for url in [good['url'].replace('/main/', '/edge/'), good['url'].replace('/x86_64/', '/aarch64/'),
                    good['url'].replace('/musl-', '/../musl-'), good['url']+'?other.apk',
                    good['url'].replace('https:', 'http:')]:
            with self.subTest(url=url), self.assertRaises(ValueError):
                m.validate_entries([dict(good, url=url)], 'x86_64', True)
    def test_required_unique_hashes(self):
        good=self.entry()
        for sha in [None, '', 'g'*64, 'a'*63]:
            with self.subTest(sha=sha), self.assertRaises(ValueError):
                m.validate_entries([dict(good, sha256=sha)], 'x86_64', True)
        with self.assertRaises(ValueError):m.validate_entries([good, good], 'x86_64', True)
        with self.assertRaises(ValueError):m.validate_entries([], 'x86_64', True)
    def test_valid_offline_cache_never_uses_network(self):
        with tempfile.TemporaryDirectory() as d:
            p=pathlib.Path(d)/'p.apk';p.write_bytes(b'valid')
            with patch.object(m.urllib.request, 'urlopen', side_effect=AssertionError('network')) as network:
                m.download('https://unused', p, hashlib.sha256(b'valid').hexdigest(), offline=True)
                network.assert_not_called()
    def test_corrupt_offline_cache_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p=pathlib.Path(d)/'p.apk';p.write_bytes(b'corrupt')
            with patch.object(m.urllib.request, 'urlopen', side_effect=AssertionError('network')) as network:
                with self.assertRaises(ValueError):m.download('https://unused', p, '0'*64, offline=True)
                self.assertEqual(p.read_bytes(), b'corrupt');network.assert_not_called()
    def test_bad_download_is_not_published(self):
        with tempfile.TemporaryDirectory() as d:
            p=pathlib.Path(d)/'p.apk'
            with patch.object(m.urllib.request, 'urlopen', side_effect=lambda *a,**kw:io.BytesIO(b'wrong')), patch.object(m.time,'sleep'):
                with self.assertRaises(ValueError):m.download('https://unused',p,'0'*64)
            self.assertFalse(p.exists());self.assertFalse(p.with_suffix('.apk.part').exists())

if __name__ == '__main__':unittest.main()
