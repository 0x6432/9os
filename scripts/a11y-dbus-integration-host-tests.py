#!/usr/bin/env python3
"""Actual shell diagnostic controls; synthetic commands, never GUI proof."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

TOP = Path(__file__).resolve().parents[1]
TEXT = (TOP/'userland/alpine/desktop-overlay/usr/bin/alpine-plasma-probe').read_text()
FUNCTION = TEXT[TEXT.index('terminal_dbus_diagnostic() {'):TEXT.index('blocked_diagnostics() {')]
POST = TEXT[TEXT.index(' echo KONSOLE_FOCUSED_TERMINAL_READY'):TEXT.index("echo 'alpine-plasma: real wallpaper/panel rendered")]
assert POST.endswith('fi\n')
POST = POST[:-3]  # outer interactive branch is not part of this extracted block

class Integration(unittest.TestCase):
    def execute(self, suffix, rc=0, enabled='0'):
        with tempfile.TemporaryDirectory() as tmp:
            mock = '''timeout() { printf '%s\\n' "$*" >> "$D/calls"; echo synthetic-observation; return "$MOCK_RC"; }\n'''
            result = subprocess.run(['/bin/sh','-eu','-c',mock+FUNCTION+'\nclient=777\n'+suffix],
                                    env={**os.environ,'D':tmp,'MOCK_RC':str(rc),'ALPINE_A11Y_DBUS_VALIDATE':enabled},
                                    text=True,capture_output=True,timeout=5)
            calls = (Path(tmp)/'calls').read_text().splitlines() if (Path(tmp)/'calls').exists() else []
            return result,calls

    def test_exact_bounded_pid_query_and_status(self):
        for rc in [0,1,2,124,143]:
            with self.subTest(rc=rc):
                r,calls=self.execute('terminal_dbus_diagnostic FAILURE_ONLY',rc)
                self.assertEqual(r.returncode,rc,r.stderr)
                self.assertEqual(calls,['-k 2 -s TERM 8 desktopa11y-dbus 777'])
                self.assertIn(f'KONSOLE_FAILURE_ONLY_DBUS_BULK_END rc={rc}',r.stdout)
                self.assertIn('synthetic-observation',r.stdout)

    def test_diagnostic_cannot_rescue_failed_gate(self):
        for rc in [0,1,124]:
            r,calls=self.execute('terminal_dbus_diagnostic FAILURE_ONLY || true\nexit 1',rc)
            self.assertEqual(r.returncode,1)
            self.assertEqual(len(calls),1)
            self.assertNotIn('KONSOLE_FOCUSED_TERMINAL_READY',r.stdout)

    def test_default_does_not_add_normal_query(self):
        r,calls=self.execute(POST)
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(calls,[])
        self.assertNotIn('DBUS_BULK',r.stdout)

    def test_explicit_post_ready_validation_adds_not_waives_gate(self):
        for rc in [0,1,124]:
            r,calls=self.execute(POST+'\necho CONTINUED',rc,enabled='1')
            self.assertEqual(r.returncode,0 if rc==0 else 1,r.stderr)
            self.assertEqual(len(calls),1)
            self.assertEqual('CONTINUED' in r.stdout,rc==0)
            self.assertLess(r.stdout.index('KONSOLE_FOCUSED_TERMINAL_READY'),r.stdout.index('KONSOLE_POST_READY_DBUS_BULK_BEGIN'))

    def test_failure_call_is_after_frozen_decision_before_unconditional_exit(self):
        failure=TEXT.index("echo 'alpine-plasma: FAIL interactive Konsole PTY/focused terminal readiness'")
        frozen=TEXT.index('echo "KONSOLE_READINESS pty=')
        diagnostic=TEXT.index('terminal_dbus_diagnostic FAILURE_ONLY || true')
        stop=TEXT.index('  exit 1',diagnostic)
        ready=TEXT.index(' echo KONSOLE_FOCUSED_TERMINAL_READY')
        self.assertLess(failure,frozen);self.assertLess(frozen,diagnostic);self.assertLess(diagnostic,stop);self.assertLess(stop,ready)
        self.assertIn('timeout -k 2 -s TERM 8 desktopa11y --terminal "$client"',TEXT[:failure])
        self.assertIn('deadline=$(( $(date +%s) + 120 ))',TEXT[:failure])

if __name__=='__main__':unittest.main()
