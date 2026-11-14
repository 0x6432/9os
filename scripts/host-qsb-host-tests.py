#!/usr/bin/env python3
"""Negative installer controls; NOT shader compilation or guest rendering proof."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

TOP = Path(__file__).resolve().parents[1]


class HostQsb(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.repo = self.base / 'repo'
        self.ports = self.repo / 'userland/ports'
        self.ports.mkdir(parents=True)
        (self.repo / 'userland/alpine').mkdir()
        for name in ['host-qsb.py', 'host-qsb-lock.json']:
            shutil.copy2(TOP / 'userland/ports' / name, self.ports / name)
        shutil.copy2(TOP / 'userland/alpine/build.py', self.repo / 'userland/alpine/build.py')
        self.lock_path = self.ports / 'host-qsb-lock.json'
        self.lock = json.loads(self.lock_path.read_text())
        self.tools = self.base / 'tools'
        self.root = self.tools / 'host/alpine-qsb-runtime-6.8.3'

    def reject(self, message):
        self.lock_path.write_text(json.dumps(self.lock, indent=2) + '\n')
        env = dict(os.environ, TOOLS_DIR=str(self.tools))
        p = subprocess.run([sys.executable, str(self.ports / 'host-qsb.py'), '--offline'],
                           env=env, capture_output=True, text=True, timeout=10)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn(message, p.stdout + p.stderr)

    def test_locked_offline_requires_cached_bootstrap(self):
        self.reject('Offline cache missing or checksum mismatch')
        self.assertFalse(self.root.exists())

    def test_mismatched_release_or_arch_rejected(self):
        for key, value in [('profile', 'desktop'), ('arch', 'aarch64'),
                           ('alpine', '3.21.0'), ('minirootfs_sha256', '0' * 64)]:
            with self.subTest(key=key):
                old = self.lock[key]
                self.lock[key] = value
                self.reject('Host qsb lock/bootstrap mismatch')
                self.lock[key] = old

    def test_untrusted_package_urls_or_missing_hashes_rejected(self):
        original = dict(self.lock['packages'][0])
        self.lock['packages'][0]['url'] = 'https://example.com/qsb.apk'
        self.reject('Package URL must name an APK')
        self.lock['packages'][0] = dict(original, sha256=None)
        self.reject('Every locked APK needs an exact SHA256')
        self.lock['packages'][0] = original
        self.lock['tool_metadata']['sha256'] = None
        self.reject('Every locked APK needs an exact SHA256')

    def test_expected_tool_version_is_mandatory(self):
        self.lock['packages'] = [e for e in self.lock['packages']
                                 if not e['url'].endswith('/qt6-qtshadertools-6.8.3-r0.apk')]
        self.reject('Expected reviewed qsb6.8.3 runtime')

    def test_unmarked_root_is_preserved(self):
        self.root.mkdir(parents=True)
        sentinel = self.root / 'keep'
        sentinel.write_text('unknown user data')
        self.reject('Refusing to replace an unmarked host tool root')
        self.assertEqual(sentinel.read_text(), 'unknown user data')

    def test_symlink_root_is_preserved(self):
        target = self.base / 'unknown'
        target.mkdir()
        self.root.parent.mkdir(parents=True)
        self.root.symlink_to(target, target_is_directory=True)
        self.reject('Refusing a symlink host tool root')
        self.assertTrue(self.root.is_symlink())
        self.assertTrue(target.is_dir())

    def test_wrong_cached_executable_version_is_rejected(self):
        self.root.mkdir(parents=True)
        # Mock cache only: never call this shader or metadata validation evidence.
        raw = (json.dumps(self.lock, indent=2) + '\n').encode()
        (self.root / '.9os-qsb-lock').write_text(hashlib.sha256(raw).hexdigest() + '\n')
        qsb = self.root / 'usr/lib/qt6/bin/qsb'
        qsb.parent.mkdir(parents=True)
        qsb.write_text('#!/bin/sh\necho "qsb 6.8.4"\n')
        qsb.chmod(0o755)
        metadata = self.root / 'usr/lib/cmake/Qt6ShaderToolsTools/Qt6ShaderToolsToolsConfig.cmake'
        metadata.parent.mkdir(parents=True)
        metadata.touch()
        self.reject('Unexpected cached qsb version')


if __name__ == '__main__':
    unittest.main()
