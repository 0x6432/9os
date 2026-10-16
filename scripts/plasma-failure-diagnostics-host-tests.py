#!/usr/bin/env python3
"""Mock only tracing; exercise actual failure PID selection, never GUI proof."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

TEXT = (Path(__file__).resolve().parents[1] / 'userland/alpine/desktop-overlay/usr/bin/alpine-plasma-probe').read_text()
FUNCTION = TEXT[TEXT.index('blocked_diagnostics() {'):TEXT.index('finish() {')]


class Diagnostics(unittest.TestCase):
    def selection(self, log, expected):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'kwin.log').write_text(log)
            commands = '''
cat() { :; }
ls() { :; }
timeout() { printf '%s\\n' "$*" >> "$TRACE"; }
'''
            script = commands + FUNCTION + '\nplasma=111;kwin=222;swaypid=333;blocked_diagnostics\n'
            result = subprocess.run(['/bin/sh', '-c', script], env=dict(os.environ, D=tmp, TRACE=tmp+'/calls'), capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('role=kwin pid='+str(expected), result.stdout)
            calls = (root / 'calls').read_text().splitlines()
            self.assertEqual(len(calls), 3)
            self.assertIn('-p '+str(expected)+' -o ', calls[1])
            self.assertTrue(all('-k 2 -s TERM 8 strace -f -tt -T' in line for line in calls))

    def test_wrapped_child(self):
        self.selection('wayland-presentation-proxy: child_pid=444\n', 444)

    def test_native(self):
        self.selection('native compositor output\n', 222)

    def test_not_shell_input(self):
        self.selection('wayland-presentation-proxy: child_pid=444;false\n', 222)

    def test_last_valid_spawn(self):
        self.selection('wayland-presentation-proxy: child_pid=444\nwayland-presentation-proxy: child_pid=555\n', 555)

    def test_no_acceptance_after_trace(self):
        self.assertIn("echo 'alpine-plasma: FAIL shell readiness';blocked_diagnostics;exit 1", TEXT)
        self.assertIn("echo 'alpine-plasma: FAIL rendered wallpaper/panel readiness'", TEXT)


if __name__ == '__main__':
    unittest.main()
