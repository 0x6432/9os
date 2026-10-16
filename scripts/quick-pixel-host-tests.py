#!/usr/bin/env python3
"""Synthetic pixel-parser controls, never guest-rendering evidence."""
import base64
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from PIL import Image

CHECKER = Path(__file__).with_name('alpine-pixel-check.py')

def frame(prefix='ALPINE_QUICK', color=(204, 51, 17, 255), size=(96, 64)):
    b = io.BytesIO()
    Image.new('RGBA', size, color).save(b, format='PNG')
    return prefix.encode() + b'_PNG_BEGIN\n' + base64.b64encode(b.getvalue()) + b'\n' + prefix.encode() + b'_PNG_END\n'

class PixelParser(unittest.TestCase):
    def check(self, raw, good, *args):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / 'console.log'
            log.write_bytes(raw)
            p = subprocess.run([sys.executable, str(CHECKER), str(log), *args], capture_output=True, timeout=10)
            self.assertEqual(p.returncode == 0, good, p.stdout + p.stderr)

    def test_unchanged_default_and_selected_source(self):
        self.check(frame(), True)
        self.check(frame('SOURCE_QUICK') * 2, True, '--prefix', 'SOURCE_QUICK', '--expected-count', '2')
        self.check(frame('SOURCE_QUICK'), False)
        self.check(frame(), False, '--prefix', 'SOURCE_QUICK')

    def test_every_selected_image_is_checked(self):
        self.check(frame() + frame(color=(0, 0, 0, 255)), False)
        self.check(frame() + frame(size=(95, 64)), False)
        self.check(frame() + frame(color=(204, 51, 17, 0)), False)

    def test_missing_truncated_and_bad_base64_rejected(self):
        self.check(b'no capture', False)
        self.check(frame() + b'ALPINE_QUICK_PNG_BEGIN\n', False)
        self.check(b'ALPINE_QUICK_PNG_END\n' + frame(), False)
        self.check(b'ALPINE_QUICK_PNG_BEGIN\n@@@\nALPINE_QUICK_PNG_END\n', False)

    def test_exact_capture_count_is_required_when_requested(self):
        self.check(frame('SOURCE_QUICK'), False, '--prefix', 'SOURCE_QUICK', '--expected-count', '2')
        self.check(frame('SOURCE_QUICK') * 3, False, '--prefix', 'SOURCE_QUICK', '--expected-count', '2')

if __name__ == '__main__':
    unittest.main()
