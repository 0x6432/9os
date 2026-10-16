#!/usr/bin/env python3
"""Execute the real shell wait with a fake clock/observer, not guest focus proof.

The separate real desktopa11y ABI controls still reject wrong PID/role/state.
This tests scheduling of that unchanged observer, never synthesizes acceptance.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / 'userland/alpine/desktop-overlay/usr/bin/alpine-plasma-probe'
ORIGINAL = ROOT / 'tests/desktop-terminal-budget/original-wait.sh'

DATE = '''#!/bin/sh
[ "$1" = +%s ] || exit 2
cat "$CLOCK"
'''
SLEEP = '''#!/bin/sh
n=$(cat "$CLOCK"); echo $((n+1)) > "$CLOCK"
'''
TIMEOUT = '''#!/bin/sh
[ "$1" = -k ] && [ "$2" = 2 ] && [ "$3" = -s ] && [ "$4" = TERM ] || exit 2
shift 4
budget=$1;shift
[ "$1" = desktopa11y ] && [ "$2" = --terminal ] || exit 2
n=$(cat "$CLOCK")
echo "$budget" >> "$CALLS"
if [ "$DURATION" -gt "$budget" ];then
 echo $((n+budget)) > "$CLOCK";exit 124
fi
echo $((n+DURATION)) > "$CLOCK"
exit "$OBSERVER_STATUS"
'''


class Budget(unittest.TestCase):
    def run_wait(self, duration=13, status=0, original=False, pty=True):
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            (d/'clock').write_text('0\n')
            for name, text in [('date', DATE), ('sleep', SLEEP), ('timeout', TIMEOUT)]:
                p = d/name;p.write_text(text);p.chmod(0o755)
            if pty:
                (d/'interactive-pty').write_text('/dev/pts/0\nKDE_INTERACTIVE_PTY\n')
            source = PROBE.read_text()
            begin = source.index(' pty_ready=0;terminal_ready=0;deadline=')
            end = source.index('\n cat "$D/konsole-focus"', begin)
            fragment = ORIGINAL.read_text() if original else source[begin:end]
            env = dict(os.environ, PATH=str(d)+':'+os.environ['PATH'], D=str(d),
                       CLOCK=str(d/'clock'), CALLS=str(d/'calls'),
                       DURATION=str(duration), OBSERVER_STATUS=str(status))
            result = subprocess.run(['sh', '-c', 'client=$$\n'+fragment+
                '\nprintf "RESULT %s %s\\n" "$pty_ready" "$terminal_ready"'],
                env=env, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            calls = [int(n) for n in (d/'calls').read_text().splitlines()] if (d/'calls').exists() else []
            return result.stdout, int((d/'clock').read_text()), calls

    def test_complete_slow_observation_within_original_deadline(self):
        out, elapsed, calls = self.run_wait()
        self.assertIn('RESULT 1 1', out)
        self.assertEqual(elapsed, 13)
        self.assertEqual(calls, [120])

    def test_original_eight_second_cap_restarts_forever(self):
        out, elapsed, calls = self.run_wait(original=True)
        self.assertIn('RESULT 1 0', out)
        self.assertGreater(len(calls), 1)
        self.assertEqual(set(calls), {8})
        self.assertGreaterEqual(elapsed, 120)

    def test_observer_exceeding_total_budget_rejected(self):
        out, elapsed, calls = self.run_wait(duration=121)
        self.assertIn('RESULT 1 0', out)
        self.assertEqual(calls, [120])
        self.assertLessEqual(elapsed, 121)  # final existing loop sleep

    def test_failed_observation_never_accepted_or_given_fresh_budget(self):
        out, elapsed, calls = self.run_wait(duration=100, status=1)
        self.assertIn('RESULT 1 0', out)
        self.assertEqual(calls, [120, 19])
        self.assertLessEqual(elapsed, 121)

    def test_no_pty_never_queries_or_accepts_terminal(self):
        out, elapsed, calls = self.run_wait(pty=False)
        self.assertIn('RESULT 0 0', out)
        self.assertEqual(calls, [])
        self.assertEqual(elapsed, 120)

    def test_fast_valid_observation_still_accepted(self):
        out, elapsed, calls = self.run_wait(duration=1)
        self.assertIn('RESULT 1 1', out)
        self.assertEqual(elapsed, 1)
        self.assertEqual(calls, [120])


if __name__ == '__main__':
    unittest.main(verbosity=2)
