#!/usr/bin/env python3
"""Actual failure-only shell function; mocks are not GUI evidence."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
TEXT=(Path(__file__).resolve().parents[1]/'userland/alpine/desktop-overlay/usr/bin/alpine-plasma-probe').read_text()
FUNCTION=TEXT[TEXT.index('register_failure_diagnostic() {'):TEXT.index('terminal_dbus_diagnostic() {')]

class Integration(unittest.TestCase):
    def test_bounded_three_samples_cannot_rescue_failure(self):
        for rc in [0,1,124,143]:
            with self.subTest(rc=rc),tempfile.TemporaryDirectory() as d:
                # Existence is real; invocation is intercepted by mock timeout.
                p=Path(d)/'trace-snapshot';p.write_text('#!/bin/sh\nexit 99\n');p.chmod(0o755)
                script='''timeout() { echo "$*" >> "$D/calls";echo synthetic-snapshot;return "$MOCK_RC"; }
sleep() { echo "sleep $*" >> "$D/calls"; }
'''+FUNCTION+'\nclient=777\nregister_failure_diagnostic || true\nexit 1\n'
                r=subprocess.run(['/bin/sh','-eu','-c',script],env={**os.environ,'D':d,'MOCK_RC':str(rc),'PATH':d+':'+os.environ['PATH']},text=True,capture_output=True,timeout=5)
                self.assertEqual(r.returncode,1,r.stderr)
                self.assertEqual((Path(d)/'calls').read_text().splitlines(),['-k 1 -s TERM 4 trace-snapshot 777','sleep 1','-k 1 -s TERM 4 trace-snapshot 777','sleep 1','-k 1 -s TERM 4 trace-snapshot 777'])
                self.assertEqual(r.stdout.count('synthetic-snapshot'),3)
                self.assertEqual(r.stdout.count(f'rc={rc}'),3)
                self.assertNotIn('KONSOLE_FOCUSED_TERMINAL_READY',r.stdout)

    def test_only_after_immutable_failure(self):
        frozen=TEXT.index('echo "KONSOLE_READINESS pty=')
        call=TEXT.index('  register_failure_diagnostic || true')
        stop=TEXT.index('  exit 1',call)
        ready=TEXT.index(' echo KONSOLE_FOCUSED_TERMINAL_READY')
        self.assertLess(frozen,call);self.assertLess(call,stop);self.assertLess(stop,ready)
        self.assertEqual(TEXT.count('  register_failure_diagnostic || true'),1)
        self.assertIn('deadline=$(( $(date +%s) + 120 ))',TEXT[:frozen])

if __name__=='__main__':unittest.main()
