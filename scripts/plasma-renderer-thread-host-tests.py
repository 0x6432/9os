#!/usr/bin/env python3
"""Session resource-policy shell controls, not graphics or startup proof."""
from pathlib import Path
import os
import subprocess
import unittest

TEXT = (Path(__file__).resolve().parents[1] / 'userland/alpine/desktop-overlay/usr/bin/alpine-plasma-probe').read_text()
LINE = next(line for line in TEXT.splitlines() if line.startswith('export LP_NUM_THREADS='))


class RendererThreads(unittest.TestCase):
    def check(self, value, expected):
        env = dict(os.environ)
        env.pop('LP_NUM_THREADS', None)
        if value is not None:
            env['LP_NUM_THREADS'] = value
        r = subprocess.run(['/bin/sh', '-c', LINE+'\nprintf "%s" "$LP_NUM_THREADS"'], env=env, capture_output=True, text=True, check=True)
        self.assertEqual(r.stdout, expected)

    def test_bus_inherits_policy_before_activation(self):
        self.assertLess(TEXT.index(LINE), TEXT.index('if dbus-run-session'))

    def test_default(self):
        self.check(None, '1')

    def test_empty_default(self):
        self.check('', '1')

    def test_override(self):
        self.check('2', '2')


if __name__ == '__main__':
    unittest.main()
