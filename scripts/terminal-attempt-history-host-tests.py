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
FAILURE_START = TEXT.index(' if [ "$pty_ready" != 1 ] || [ "$terminal_ready" != 1 ];then')
FAILURE_BRANCH = TEXT[FAILURE_START:TEXT.index(' echo KONSOLE_FOCUSED_TERMINAL_READY', FAILURE_START)]
TERMINAL = 'DBUS_A11Y_TERMINAL focused=0 showing=1 visible=1\n'
REMOTE = 'DBUS_A11Y_ERROR reason=remote-call\n'
WITNESS = ('DBUS_A11Y_WITNESS pid=777 terminals=1 valid_states=1 focused_states=0 '
           'showing_states=1 visible_states=1 qualified=0 protocol_error=1\n')
FINAL = 'terminal-dbus: pid=777 applications=1 models=0 visited=12 NOT_OBSERVED (diagnostic only)\n'
MOCKS = r'''
set -eu
client=777
# The real probe initializes these before its failure branch. Explicitly seed
# them here so POSIX shells can expand every argument before calling timeout.
parent=stub-wayland
output=stub-output
NOW=100
DEADLINE=220
printf '0\n' > "$D/date-calls"
printf '0\n' > "$D/queries"
printf '0\n' > "$D/diagnostic-calls"
cat() {
 case "${1:-}" in /proc/*) return 0 ;; esac
 command cat "$@"
}
date() {
 n=$(cat "$D/date-calls");n=$((n+1));printf '%s\n' "$n" > "$D/date-calls"
 printf '%s\n' "$NOW"
 if [ "$date_fail_call" != 0 ] && [ "$n" = "$date_fail_call" ];then return 1;fi
 return 0
}
sleep() { :; }
kill() {
 n=$(cat "$D/queries")
 [ "$pty_ready" = 1 ] && [ "$n" -lt "$fixture_count" ]
}
timeout() {
 case " $* " in *' grim '*|*' strace '*) return 1 ;; esac
 printf '%s\n' "$*" >> "$D/calls"
 n=$(cat "$D/queries");n=$((n+1));printf '%s\n' "$n" > "$D/queries"
 NOW=$(cat "$D/completion-$n")
 cat "$D/input-$n"
 rc=$(cat "$D/rc-$n")
 return "$rc"
}
register_failure_diagnostic() { :; }
terminal_dbus_diagnostic() {
 n=$(cat "$D/diagnostic-calls");n=$((n+1));printf '%s\n' "$n" > "$D/diagnostic-calls"
 printf 'STUB_FAILURE_DIAGNOSTIC rc=%s\n' "$diagnostic_rc"
 return "$diagnostic_rc"
}
ps() { printf 'stub process list\n'; }
ls() { :; }
tail() { :; }
'''

TEST_POST = r'''
printf 'RESULT pty=%s terminal=%s\n' "$pty_ready" "$terminal_ready"
if [ "$pty_ready" != 1 ] || [ "$terminal_ready" != 1 ];then exit 1;fi
'''


def execute(attempts, backend='live-dbus', pty=True, functions=FUNCTIONS,
            loop=LOOP, override='', completion_times=None, date_fail_call=0,
            actual_failure_branch=False, diagnostic_rc=0):
    with tempfile.TemporaryDirectory(prefix='terminal-history-') as tmp:
        root = Path(tmp)
        (root/'queries').write_text('0')
        (root/'date-calls').write_text('0')
        (root/'diagnostic-calls').write_text('0')
        if pty:
            (root/'interactive-pty').write_text('/dev/pts/mock\nKDE_INTERACTIVE_PTY\n')
        if completion_times is None:
            completion_times = [100] * len(attempts)
        if len(completion_times) != len(attempts):
            raise ValueError('one completion time is required per observer fixture')
        for n, ((rc, text), completion) in enumerate(zip(attempts, completion_times), 1):
            (root/f'input-{n}').write_text(text)
            (root/f'rc-{n}').write_text(str(rc))
            (root/f'completion-{n}').write_text(str(completion))
        (root/'konsole.log').write_text('stub client log\n')
        suffix = (FAILURE_BRANCH+'\necho KONSOLE_FOCUSED_TERMINAL_READY\n'
                  if actual_failure_branch else TEST_POST)
        script = (MOCKS+'\n'+functions+'\n'+override+'\n'+loop+'\n'+suffix)
        r = subprocess.run(['/bin/sh', '-eu', '-c', script], capture_output=True, text=True,
                           timeout=10, env={**os.environ, 'D':tmp,
                                            'fixture_count':str(len(attempts)),
                                            'ALPINE_TERMINAL_QUERY':backend,
                                            'date_fail_call':str(date_fail_call),
                                            'diagnostic_rc':str(diagnostic_rc)})
        r.date_calls = int((root/'date-calls').read_text())
        r.query_calls = int((root/'queries').read_text())
        r.diagnostic_calls = int((root/'diagnostic-calls').read_text())
        slots = {p.name.removeprefix('konsole-attempt-'):p.read_text()
                 for p in root.glob('konsole-attempt-*') if p.is_file()}
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

    def test_existing_retry_success_retains_failure_then_success_without_more_queries(self):
        ready_terminal = TERMINAL.replace('focused=0', 'focused=1')
        ready_witness = WITNESS.replace('focused_states=0', 'focused_states=1').replace('qualified=0', 'qualified=1').replace('protocol_error=1', 'protocol_error=0')
        ready_final = FINAL.replace('models=0', 'models=1').replace('NOT_OBSERVED', 'OBSERVED')
        r, slots, calls = execute([(1, TERMINAL+REMOTE+WITNESS+FINAL),
                                    (0, ready_terminal+ready_witness+ready_final), (142, '')])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len(calls), 2)
        self.assertIn('attempt=1 rc=1', slots['first'])
        self.assertIn('attempt=2 rc=0', slots['last'])
        self.assertIn('focused=0', slots['terminal-first'])
        self.assertIn('focused=1', slots['terminal-last'])
        self.assertIn('protocol_error=1', slots['witness-first'])
        self.assertIn('protocol_error=0', slots['witness-last'])
        self.assertIn('attempt=1 rc=1', slots['remote-last'])
        self.assertNotIn('alarm-first', slots)

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

    def test_foreign_backend_terminal_error_lines_remain_raw_not_selected_observations(self):
        for backend, text in [('atspi', TERMINAL+REMOTE),
                              ('live-dbus', TERMINAL.replace('DBUS_A11Y','A11Y'))]:
            r, slots, calls = execute([(1, text)], backend=backend)
            self.assertEqual(r.returncode, 1, r.stderr)
            self.assertEqual(len(calls), 1)
            self.assertIn('terminal_line=unknown remote_line=unknown', slots['first'])
            self.assertIn('backend='+backend+' pid=777', slots['first'])
            self.assertNotIn('terminal-first', slots)
            self.assertNotIn('remote-first', slots)
            self.assertIn(text.strip(), slots['first'])  # retain original raw evidence

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
        self.assertLess(len(r.stdout.encode()), 128*1024)  # hex history framing/payload bound
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

    def test_missing_or_unwritable_recording_is_reported_not_accepted(self):
        # Actual recorder I/O failures, not just an overridden return code.
        for setup in ['mkdir "$D/konsole-attempt-meta"',
                      'mkdir "$D/konsole-attempt-last"',
                      'cat() { return 1; }']:
            # Override cat only inside the recorder in the third case; queries
            # still run the real synthetic helper and its original exit status.
            functions = FUNCTIONS
            if setup.startswith('cat()'):
                functions = functions.replace('cat "$D/konsole-attempt-meta"', 'false')
                setup = ''
            for rc in [0, 1]:
                r, _, calls = execute([(rc, TERMINAL)], functions=functions, override=setup)
                self.assertEqual(r.returncode, 0 if rc==0 else 1, r.stderr)
                self.assertIn('recording_failed=1', r.stdout)
                self.assertEqual(len(calls), 1)

    def test_legacy_cold_style_final_is_not_a_zero_count_witness(self):
        r, slots, _ = execute([(1, TERMINAL+REMOTE+FINAL)])
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn('completion=present witness=unknown', slots['first'])
        self.assertIn('remote_line=present', slots['remote-first'])
        self.assertIn('terminal_first='+TERMINAL.strip(), slots['terminal-first'])
        self.assertNotIn('witness-first', slots)
        self.assertNotIn('qualified=0', slots['first'])

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
        self.assertLess(LOOP.index('else query_rc=$?;fi'), LOOP.index('query_in_deadline=0'))
        self.assertLess(LOOP.index('query_in_deadline=0'), LOOP.index('record_terminal_attempt'))
        self.assertLess(LOOP.index('record_terminal_attempt'), LOOP.index('if [ "$query_in_deadline" = 1 ]'))
        self.assertIn('if query_now=$(date +%s) && [ "$query_now" -lt "$deadline" ];then query_in_deadline=1;fi', LOOP)
        self.assertNotIn('if [ "$query_rc" = 0 ];then terminal_ready=1;break;fi', LOOP)

    def test_exact_and_late_success_fail_exclusive_deadline(self):
        witness = TERMINAL.replace('focused=0','focused=1')+WITNESS.replace('focused_states=0','focused_states=1').replace('qualified=0','qualified=1').replace('protocol_error=1','protocol_error=0')+FINAL.replace('models=0','models=1').replace('NOT_OBSERVED','OBSERVED')
        for completed in (220, 221):
            with self.subTest(completed=completed):
                r, slots, calls = execute([(0, witness)], completion_times=[completed])
                self.assertEqual(r.returncode, 1, r.stdout+r.stderr)
                self.assertEqual(r.query_calls, 1)
                self.assertEqual(r.date_calls, 3)  # init, loop-entry, successful-query completion
                self.assertIn('attempt=1 rc=0', slots['first'])
                self.assertIn('RESULT pty=1 terminal=0', r.stdout)
                self.assertEqual(len(calls), 1)

    def test_failed_query_then_timely_success_retains_both_attempts(self):
        ready_terminal = TERMINAL.replace('focused=0','focused=1')
        ready_witness = WITNESS.replace('focused_states=0','focused_states=1').replace('qualified=0','qualified=1').replace('protocol_error=1','protocol_error=0')
        ready_final = FINAL.replace('models=0','models=1').replace('NOT_OBSERVED','OBSERVED')
        r, slots, calls = execute([(1, TERMINAL+REMOTE+WITNESS+FINAL),
                                   (0, ready_terminal+ready_witness+ready_final)],
                                  completion_times=[110, 219])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.query_calls, 2)
        self.assertEqual(len(calls), 2)
        self.assertEqual(r.date_calls, 4)  # failed query skips the post-query date sample
        self.assertIn('attempt=1 rc=1', slots['first'])
        self.assertIn('attempt=2 rc=0', slots['last'])
        self.assertIn('focused=0', slots['terminal-first'])
        self.assertIn('focused=1', slots['terminal-last'])

    def test_failed_post_query_date_call_fails_closed(self):
        ready_terminal = TERMINAL.replace('focused=0','focused=1')
        ready_witness = WITNESS.replace('focused_states=0','focused_states=1').replace('qualified=0','qualified=1').replace('protocol_error=1','protocol_error=0')
        ready_final = FINAL.replace('models=0','models=1').replace('NOT_OBSERVED','OBSERVED')
        r, slots, _ = execute([(0, ready_terminal+ready_witness+ready_final)],
                              completion_times=[219], date_fail_call=3)
        self.assertEqual(r.returncode, 1, r.stdout+r.stderr)
        self.assertEqual(r.query_calls, 1)
        self.assertEqual(r.date_calls, 3)
        self.assertIn('attempt=1 rc=0', slots['first'])
        self.assertIn('RESULT pty=1 terminal=0', r.stdout)

    def test_late_success_cannot_be_rescued_by_actual_production_failure_branch(self):
        text = TERMINAL.replace('focused=0','focused=1')+WITNESS.replace('focused_states=0','focused_states=1').replace('qualified=0','qualified=1').replace('protocol_error=1','protocol_error=0')+FINAL.replace('models=0','models=1').replace('NOT_OBSERVED','OBSERVED')
        r, _, _ = execute([(0, text)], completion_times=[221],
                          actual_failure_branch=True, diagnostic_rc=0)
        self.assertEqual(r.returncode, 1, r.stdout+r.stderr)
        self.assertIn('alpine-plasma: FAIL interactive Konsole PTY/focused terminal readiness', r.stdout)
        self.assertIn('STUB_FAILURE_DIAGNOSTIC rc=0', r.stdout)
        self.assertEqual(r.diagnostic_calls, 1)
        self.assertNotIn('KONSOLE_FOCUSED_TERMINAL_READY', r.stdout)

    def test_guard_removal_and_inclusive_boundary_mutants_are_detected(self):
        guarded = ('   query_in_deadline=0\n'
                   '   if [ "$query_rc" = 0 ];then\n'
                   '    if query_now=$(date +%s) && [ "$query_now" -lt "$deadline" ];then query_in_deadline=1;fi\n'
                   '   fi\n'
                   '   record_terminal_attempt || query_recording_failed=1\n'
                   '   if [ "$query_in_deadline" = 1 ];then terminal_ready=1;break;fi\n')
        removed = ('   record_terminal_attempt || query_recording_failed=1\n'
                   '   if [ "$query_rc" = 0 ];then terminal_ready=1;break;fi\n')
        no_guard = LOOP.replace(guarded, removed)
        self.assertNotEqual(no_guard, LOOP)
        late = [(0, TERMINAL+WITNESS+FINAL)]
        good, _, _ = execute(late, loop=LOOP, completion_times=[221])
        bad, _, _ = execute(late, loop=no_guard, completion_times=[221])
        self.assertEqual(good.returncode, 1)
        self.assertEqual(bad.returncode, 0)  # mutant restores late acceptance
        inclusive = LOOP.replace('[ "$query_now" -lt "$deadline" ]', '[ "$query_now" -le "$deadline" ]')
        self.assertNotEqual(inclusive, LOOP)
        good, _, _ = execute(late, loop=LOOP, completion_times=[220])
        bad, _, _ = execute(late, loop=inclusive, completion_times=[220])
        self.assertEqual(good.returncode, 1)
        self.assertEqual(bad.returncode, 0)  # mutant wrongly accepts the boundary


if __name__ == '__main__':
    unittest.main()
