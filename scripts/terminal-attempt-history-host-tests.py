#!/usr/bin/env python3
"""Execute actual POSIX shell readiness loop/retention; NOT GUI or cold proof.

No extra acceptance queries. Tests use synthetic helpers, including mutations of
real production shell. Retained raw slices overlap deliberately on short logs.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TEXT = (ROOT/'userland/alpine/desktop-overlay/usr/bin/alpine-plasma-probe').read_text()
FUNCTIONS = TEXT[TEXT.index('focused_terminal_query() {'):TEXT.index('# Diagnostic only: all samples')]
LOOP = TEXT[TEXT.index(' query_attempt=0;'):TEXT.index(' if [ "$pty_ready" != 1 ]')]
TERMINAL = 'DBUS_A11Y_TERMINAL focused=0 showing=1 visible=1\n'
REMOTE = 'DBUS_A11Y_ERROR reason=remote-call\n'
WITNESS = ('DBUS_A11Y_WITNESS pid=777 terminals=1 valid_states=1 focused_states=0 '
           'showing_states=1 visible_states=1 qualified=0 protocol_error=1\n')
FINAL = 'terminal-dbus: pid=777 applications=1 models=0 visited=12 NOT_OBSERVED (diagnostic only)\n'
MOCKS = r'''
client=777
# First date sets deadline220; subsequent calls allow exactly fixture count.
date() {
 n=$(cat "$D/dates"); n=$((n+1)); echo "$n" > "$D/dates"
 if [ "$n" -le "$fixture_count" ];then echo 100;else echo 220;fi
}
sleep() { :; }
kill() { return 0; }
timeout() {
 printf '%s\n' "$*" >> "$D/calls"
 n=$(cat "$D/queries"); n=$((n+1)); echo "$n" > "$D/queries"
 cat "$D/input-$n"
 return "$(cat "$D/rc-$n")"
}
'''


def execute(attempts, backend='live-dbus', pty=True, functions=FUNCTIONS, loop=LOOP, override=''):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root/'dates').write_text('-1')
        (root/'queries').write_text('0')
        if pty:
            (root/'interactive-pty').write_text('/dev/pts/mock\nKDE_INTERACTIVE_PTY\n')
        for n, (rc, text) in enumerate(attempts, 1):
            (root/f'input-{n}').write_text(text)
            (root/f'rc-{n}').write_text(str(rc))
        script = MOCKS+functions+'\n'+override+'\n'+loop+'''\nprintf 'RESULT pty=%s terminal=%s\n' "$pty_ready" "$terminal_ready"
if [ "$pty_ready" != 1 ] || [ "$terminal_ready" != 1 ];then exit 1;fi
'''
        r = subprocess.run(['/bin/sh', '-eu', '-c', script], capture_output=True, text=True,
                           timeout=10, env={**os.environ, 'D':tmp, 'fixture_count':str(len(attempts)),
                                            'ALPINE_TERMINAL_QUERY':backend})
        slots = {p.name.removeprefix('konsole-attempt-'):p.read_text() for p in root.glob('konsole-attempt-*')}
        calls = (root/'calls').read_text().splitlines() if (root/'calls').exists() else []
        return r, slots, calls


class History(unittest.TestCase):
    def test_failed_sequence_preserves_first_terminal_remote_and_incomplete(self):
        r, slots, calls = execute([(1, TERMINAL+REMOTE+WITNESS+FINAL), (142, 'unfinished\n'), (124, '')])
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertEqual(len(calls), 3)
        self.assertEqual(calls, ['-k 2 -s TERM 8 desktopa11y-dbus 777']*3)
        self.assertIn('attempt=1 rc=1', slots['first'])
        self.assertIn('attempt=3 rc=124 termination=timeout-status', slots['last'])
        self.assertIn('terminal_first='+TERMINAL.strip(), slots['terminal-first'])
        self.assertIn('remote_line=present', slots['remote-first'])
        self.assertIn('witness_line='+WITNESS.strip(), slots['witness-first'])
        self.assertIn('completion=present witness=present', slots['first'])
        self.assertIn('attempt=2 rc=142 termination=alarm-status', slots['alarm-first'])
        self.assertIn('completion=unknown witness=unknown terminal_line=unknown remote_line=unknown', slots['last'])
        self.assertIn('attempt=2', slots['incomplete-first'])
        self.assertIn('attempt=3', slots['incomplete-last'])
        self.assertNotIn('KONSOLE_FOCUSED_TERMINAL_READY', r.stdout)

    def test_no_pty_no_queries_no_fabricated_observations(self):
        r, slots, calls = execute([(1, TERMINAL)], pty=False)
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertEqual(calls, [])
        self.assertEqual(slots, {})
        self.assertIn('attempts=0 recording_failed=0', r.stdout)
        self.assertNotIn('terminal_line=', r.stdout)

    def test_positive_stops_on_original_rc_not_diagnostic_claims(self):
        for backend, command in [('live-dbus','desktopa11y-dbus 777'), ('atspi','desktopa11y --terminal 777')]:
            # No witness, even no text: do NOT introduce a new acceptance predicate.
            r, slots, calls = execute([(0, ''), (1, TERMINAL)], backend=backend)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(calls, ['-k 2 -s TERM 8 '+command])
            self.assertIn('completion=unknown witness=unknown', slots['first'])
            self.assertIn('RESULT pty=1 terminal=1', r.stdout)
        r, _, _ = execute([(1, TERMINAL.replace('focused=0','focused=1')+WITNESS+FINAL)])
        self.assertEqual(r.returncode, 1)  # raw claimed bits never rescue original failure

    def test_exact_lines_pid_backend_and_legacy_completion(self):
        for text in ['prefix '+WITNESS, WITNESS.replace('777','778'), WITNESS.rstrip()+' extra\n',
                     'DBUS_A11Y_WITNESS pid=777\n', 'prefix '+TERMINAL, REMOTE.rstrip()+' extra\n']:
            r, slots, _ = execute([(1, text)])
            self.assertEqual(r.returncode, 1, r.stderr)
            self.assertIn('completion=unknown witness=unknown terminal_line=unknown remote_line=unknown', slots['first'])
        r, slots, _ = execute([(1, WITNESS+FINAL)], backend='atspi')
        self.assertIn('completion=unknown witness=unknown', slots['first'])
        r, slots, calls = execute([(1, 'A11Y_TERMINAL focused=0 showing=1 visible=1\n'
                                  'desktopa11y: pid=777 application=1 models=0 visited=12 NOT_READY\n')], backend='atspi')
        self.assertIn('completion=present witness=unknown terminal_line=present', slots['first'])
        self.assertNotIn('incomplete-first', slots)

    def test_large_middle_observation_retained_and_storage_output_bounded(self):
        # More iterations cannot grow slot count/size; significant mid-log lines
        # survive independently of bounded raw prefix/suffix. One long line too.
        text = 'BEGIN\n'+'x'*16000+'\n'+TERMINAL+WITNESS+REMOTE+'y'*16000+'\n'+FINAL
        r, slots, calls = execute([(142, text)]*40+[(124, '')])
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertEqual(len(calls), 41)
        self.assertLessEqual(len(slots), 13)  #12 slots + one <=1024-byte metadata scratch
        for slot, content in slots.items():
            self.assertLessEqual(len(content.encode()), 1024 if slot=='meta' else 5120, slot)
        self.assertLessEqual(sum(len(s.encode()) for s in slots.values()), 12*5120+1024)
        self.assertLess(len(r.stdout.encode()), 65000)
        self.assertIn('terminal_first='+TERMINAL.strip(), slots['terminal-last'])
        self.assertIn('witness_line='+WITNESS.strip(), slots['witness-last'])
        self.assertIn('attempt=1 ', slots['alarm-first'])
        self.assertIn('attempt=40 ', slots['alarm-last'])
        self.assertIn('attempt=41 ', slots['last'])

    def test_status_distinctions_not_all_nonzero_are_alarm(self):
        for rc, termination in [(1,'exit-status'), (2,'exit-status'), (124,'timeout-status'),
                                (137,'signal-status'), (142,'alarm-status'), (143,'signal-status')]:
            r, slots, _ = execute([(rc, '')])
            self.assertEqual(r.returncode, 1, r.stderr)
            self.assertIn(f'rc={rc} termination={termination}', slots['first'])
            self.assertEqual('alarm-first' in slots, rc==142)

    def test_recording_error_does_not_change_acceptance_or_exit(self):
        for rc in [0,1,124,142]:
            r, slots, calls = execute([(rc, '')], override='record_terminal_attempt() { return 1; }')
            self.assertEqual(r.returncode, 0 if rc==0 else 1, r.stderr)
            self.assertIn('recording_failed=1', r.stdout)
            self.assertEqual(len(calls), 1)
            self.assertEqual(slots, {})

    def test_mutations_are_detected(self):
        variants = [
            (FUNCTIONS.replace('[ ! -f "$D/konsole-attempt-$query_slot" ] || continue', ':'),
             [(1, TERMINAL), (124, '')], lambda slots: 'attempt=1 ' in slots['first']),
            (FUNCTIONS.replace('witness="unknown"', 'witness="zero"'),
             [(142, '')], lambda slots: 'witness=unknown' in slots['first']),
            (FUNCTIONS.replace('142) query_end=alarm-status', '1|142) query_end=alarm-status'),
             [(1, '')], lambda slots: 'termination=exit-status' in slots['first']),
            (FUNCTIONS.replace('head -c 2048 "$D/konsole-focus"', 'cat "$D/konsole-focus"'),
             [(1, 'x'*20000)], lambda slots: len(slots['first'])<=5120),
        ]
        for functions, attempts, assertion in variants:
            self.assertNotEqual(functions, FUNCTIONS)
            _, good, _ = execute(attempts)
            _, mutated, _ = execute(attempts, functions=functions)
            self.assertTrue(assertion(good))
            self.assertFalse(assertion(mutated))

    def test_unchanged_query_deadline_and_order(self):
        self.assertEqual(LOOP.count('if focused_terminal_query "$client"'), 1)
        self.assertIn('deadline=$(( $(date +%s) + 120 ))', LOOP)
        self.assertIn('sleep .1', LOOP)
        self.assertLess(LOOP.index('pty_ready=1'), LOOP.index('if focused_terminal_query'))
        self.assertLess(LOOP.index('else query_rc=$?;fi'), LOOP.index('record_terminal_attempt'))
        self.assertIn('if [ "$query_rc" = 0 ];then terminal_ready=1;break;fi', LOOP)


if __name__ == '__main__':
    unittest.main()
