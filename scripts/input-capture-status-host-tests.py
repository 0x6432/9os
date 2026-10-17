#!/usr/bin/env python3
"""Actual-shell completion publication controls; not hardware input evidence."""
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

HELPER = Path(__file__).resolve().parents[1] / 'userland/alpine/desktop-overlay/usr/bin/input-capture-session'


class Publication(unittest.TestCase):
    def run_case(self, rc, old=False):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            mock = root / 'inputeventcapture'
            mock.write_text('#!/bin/sh\necho INPUT_CAPTURE_READY devices=1\n'
                            'while [ ! -e "$RELEASE" ]; do sleep .01; done\n'
                            'echo INPUT_CAPTURE_DONE events=0 repeats=0 dropped=0 error=0\n'
                            'exit "$MOCK_RC"\n')
            mock.chmod(0o755)
            env = dict(os.environ, PATH=str(root)+':'+os.environ['PATH'],
                       RELEASE=str(root/'release'), MOCK_RC=str(rc))
            log, status = root/'capture.log', root/'status'
            args = ['/bin/sh', str(HELPER), str(log), str(status), '45']
            if old:
                args = ['/bin/sh', '-c', '(if inputeventcapture 45 >"$1" 2>&1;then echo 0;else echo $?;fi) >"$2"', 'old', str(log), str(status)]
            proc = subprocess.Popen(args, env=env)
            try:
                deadline = time.monotonic()+5
                while not (log.exists() and 'INPUT_CAPTURE_READY' in log.read_text()):
                    if proc.poll() is not None or time.monotonic()>deadline:
                        self.fail('mock did not reach blocked collector readiness')
                    time.sleep(.01)
                self.assertIsNone(proc.poll())
                if old:
                    self.assertTrue(status.exists())
                    self.assertEqual(status.read_text(), '')
                else:
                    self.assertFalse(status.exists())
                    self.assertFalse(Path(str(status)+'.tmp').exists())
                (root/'release').touch()
                self.assertEqual(proc.wait(timeout=5), 0 if old else rc)
                self.assertEqual(status.read_text(), str(rc)+'\n')
                self.assertIn('INPUT_CAPTURE_DONE', log.read_text())
                self.assertFalse(Path(str(status)+'.tmp').exists())
            finally:
                if proc.poll() is None:
                    (root/'release').touch()
                    proc.kill()
                    proc.wait()

    def test_success_is_not_published_early(self):
        self.run_case(0)

    def test_failure_is_not_published_early(self):
        self.run_case(7)

    def test_original_redirect_exposes_empty_status_early(self):
        self.run_case(0, old=True)

    def test_existing_status_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as root:
            status = Path(root)/'status'
            status.write_text('previous\n')
            result = subprocess.run(['/bin/sh', str(HELPER), str(Path(root)/'log'), str(status), '45'])
            self.assertEqual(result.returncode, 2)
            self.assertEqual(status.read_text(), 'previous\n')


if __name__ == '__main__':
    unittest.main()
