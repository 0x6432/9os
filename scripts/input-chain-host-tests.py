#!/usr/bin/env python3
"""Native host controls for input-chain diagnostics; no VM or target build."""
from importlib.machinery import SourceFileLoader
from pathlib import Path
import json
import os
import re
import signal
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
CHECK = SourceFileLoader('input_chain_check', str(Path(__file__).with_name('input-chain-check.py'))).load_module()
READY = SourceFileLoader('qemu_ready_type', str(Path(__file__).with_name('qemu-ready-type.py'))).load_module()
RAW = CHECK.RAW
EXPECTED = [edge for frame in RAW.expected_frames(RAW.DEFAULT_TEXT) for edge in frame]
CAPTURE_READY = ROOT / 'userland/alpine/desktop-overlay/usr/bin/input-capture-ready'


def raw_records():
    lines = ['INPUT_CAPTURE_DEVICE index=1 named=1 monotonic=1 name=QEMU Virtio Keyboard',
             'INPUT_CAPTURE_READY devices=1']
    rows = []
    for frame_index, frame in enumerate(RAW.expected_frames(RAW.DEFAULT_TEXT)):
        rows.extend((1, code, value) for code, value in frame)
        rows.append((0, 0, 0))
    for index, (event_type, code, value) in enumerate(rows):
        lines.append('INPUT_CAPTURE_EVENT device=1 sec=10 usec=%d type=%d code=%d value=%d received_ns=%d' %
                     (index, event_type, code, value, 1000000000 + index * 1000))
    lines.append('INPUT_CAPTURE_DONE events=%d repeats=0 dropped=0 error=0' % len(rows))
    return '\n'.join(lines)


def outer_trace(edges=EXPECTED):
    lines = [
        'WAYLAND_KEY_TRACE object=9 event=repeat_info received_ns=1000000000 rate=25 delay=600',
        'WAYLAND_KEY_TRACE object=9 event=repeat_info received_ns=1000000001 rate=25 delay=600',
    ]
    for index, (code, state) in enumerate(edges):
        lines.append('WAYLAND_KEY_TRACE object=9 event=key received_ns=%d serial=%d time_ms=%d key=%d state=%d' %
                     (1000000002 + index, index + 1, index, code, state))
    return '\n'.join(lines)


def inner_trace(edges=EXPECTED):
    lines = ['[1234567.123] {Default Queue} wl_keyboard#26.repeat_info(25, 600)']
    lines.extend('[1234567.123] {Default Queue} wl_keyboard#26.key(%d, %d, %d, %d)' %
                 (index + 1, index, code, state) for index, (code, state) in enumerate(edges))
    return 'INNER_KEYBOARD_DISPATCH_TRACE_BEGIN\n' + '\n'.join(lines) + '\nINNER_KEYBOARD_DISPATCH_TRACE_END\n'


def qt_trace(edges=EXPECTED, repeat=False):
    lines = [CHECK.QT.START, CHECK.QT.LOADED]
    events = list(edges)
    repeat_index = len(events)
    if repeat:
        events.append((20, 1))
    for index, (code, state) in enumerate(events):
        autorepeat = int(repeat and index == repeat_index)
        for offset, phase, result in ((0, 'begin', -1), (1, 'end', 1)):
            lines.append(
                'QT_KEY_TRACE seq=%d phase=%s pid=900 clock_ok=1 mono_ns=%d source_ms=12345 '
                'type=%d key=68 modifiers=0 scan=%d sym=100 native=0 autorepeat=%d count=1 result=%d' %
                (index, phase, index * 2 + offset + 1, 6 if state else 7, code + 8, autorepeat, result))
    lines.append(CHECK.QT.END)
    return '\n'.join(lines) + '\n'


def transcript(*, raw_status='0', outer=None, inner=None, qt=None, started=True,
               raw=None, capture_ready='INPUT_CHAIN_CAPTURE_READY devices=1'):
    outer = outer_trace() if outer is None else outer
    inner = inner_trace() if inner is None else inner
    qt = qt_trace() if qt is None else qt
    raw = raw_records() if raw is None else raw
    return (
        'INPUT_CHAIN_DIAGNOSTICS_ENABLED\n'
        + ('INPUT_CHAIN_CAPTURE_STARTED seconds=45\n' if started else '')
        + capture_ready + '\n'
        + CHECK.OUTER_BEGIN + '\n' + outer + '\n' + CHECK.OUTER_END + '\n'
        + inner + qt
        + CHECK.RAW_BEGIN + '\nINPUT_CHAIN_RAW_STATUS=' + raw_status + '\n'
        + raw + '\n' + CHECK.RAW_END + '\n'
    )


class ChainCheck(unittest.TestCase):
    def test_exact_one_attempt_all_boundaries(self):
        result = CHECK.check(transcript())
        self.assertEqual(result['raw']['taps'], 128)
        self.assertEqual(result['raw']['key_events'], 262)
        self.assertEqual(result['outer']['key_edges'], 262)
        self.assertEqual(result['inner']['key_edges'], 262)
        self.assertEqual(result['qt']['physical_key_edges'], 262)
        self.assertEqual(result['qt']['autorepeat_events'], 0)

    def test_outer_extra_missing_truncated_and_nonmonotonic_are_rejected(self):
        good = outer_trace()
        variants = [
            good.replace('key=20 state=1', 'key=21 state=1', 1),
            good + '\nWAYLAND_KEY_TRACE truncated=1',
            good.replace('received_ns=1000000003', 'received_ns=1', 1),
            good.replace('event=repeat_info', 'event=unknown', 1),
        ]
        for value in variants:
            with self.subTest(value=value[:100]), self.assertRaises(ValueError):
                CHECK.check(transcript(outer=value))

    def test_outer_receipt_overflow_is_rejected(self):
        bad = outer_trace().replace('received_ns=1000000002',
                                    'received_ns=18446744073709551616', 1)
        with self.assertRaisesRegex(ValueError, 'uint64'):
            CHECK.check(transcript(outer=bad))

    def test_missing_or_duplicate_frame_blocks_are_rejected(self):
        good = transcript()
        duplicate_outer = good.replace(CHECK.OUTER_END, CHECK.OUTER_END + '\n' +
                                       CHECK.OUTER_BEGIN + '\n' +
                                       good.split(CHECK.OUTER_BEGIN + '\n', 1)[1].split(CHECK.OUTER_END, 1)[0] +
                                       '\n' + CHECK.OUTER_END, 1)
        self.assertRaises(ValueError, CHECK.check, duplicate_outer)
        self.assertRaises(ValueError, CHECK.check, transcript(started=False))

    def test_raw_status_and_inner_edges_are_not_silently_accepted(self):
        self.assertRaises(ValueError, CHECK.check, transcript(raw_status='7'))
        bad_inner = inner_trace().replace('.key(1, 0, 20, 1)', '.key(1, 0, 21, 1)', 1)
        self.assertRaises(ValueError, CHECK.check, transcript(inner=bad_inner))

    def test_raw_ready_error_malformed_and_device_mismatch_are_rejected(self):
        good = raw_records()
        cases = (
            good.replace('INPUT_CAPTURE_READY devices=1\n', ''),
            good.replace('INPUT_CAPTURE_READY devices=1', 'INPUT_CAPTURE_READY devices=x'),
            good.replace('INPUT_CAPTURE_READY devices=1', 'INPUT_CAPTURE_READY devices=2'),
            good.replace('INPUT_CAPTURE_READY devices=1',
                         'INPUT_CAPTURE_ERROR no devices\nINPUT_CAPTURE_READY devices=1'),
        )
        for raw in cases:
            with self.subTest(raw=raw[:100]), self.assertRaises(ValueError):
                CHECK.check(transcript(raw=raw))

    def test_raw_abi_overflows_are_rejected_before_shared_checker(self):
        good = raw_records()
        cases = (
            ('received_ns=1000000000', 'received_ns=18446744073709551616'),
            ('sec=10', 'sec=9223372036854775808'),
            ('type=1', 'type=65536'),
            ('value=1', 'value=2147483648'),
            ('device=1', 'device=32'),
        )
        for before, after in cases:
            with self.subTest(after=after):
                bad = good.replace(before, after, 1)
                with self.assertRaises(ValueError):
                    CHECK.check(transcript(raw=bad))

    def test_readiness_witness_must_be_valid_and_match_retained_raw(self):
        for witness in ('INPUT_CHAIN_CAPTURE_READY_ERROR timeout',
                        'INPUT_CHAIN_CAPTURE_READY devices=33',
                        'INPUT_CHAIN_CAPTURE_READY devices=2',
                        'INPUT_CHAIN_CAPTURE_READY devices=x'):
            with self.subTest(witness=witness), self.assertRaises(ValueError):
                CHECK.check(transcript(capture_ready=witness))

    def test_qt_autorepeat_is_retained_and_rejected_in_strict_chain(self):
        self.assertRaises(ValueError, CHECK.check, transcript(qt=qt_trace(repeat=True)))

    def test_result_scope_disclaims_authenticated_provenance_and_splice_rejection(self):
        scope = CHECK.check(transcript())['scope']
        self.assertIn('producer provenance is not authenticated', scope)
        self.assertIn('spliced-log rejection is not provided', scope)


class CaptureReadiness(unittest.TestCase):
    def run_ready(self, log, polls='1', status=None, env=None):
        command = ['/bin/sh', str(CAPTURE_READY), str(log), polls]
        if status is not None:
            command.append(str(status))
        return subprocess.run(command, capture_output=True, text=True, timeout=8, env=env)

    def test_ready_after_devices_open_succeeds(self):
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp) / 'raw.log'
            log.write_text('INPUT_CAPTURE_DEVICE index=1 named=1 monotonic=1 name=QEMU Virtio Keyboard\n'
                           'INPUT_CAPTURE_READY devices=1\n')
            result = self.run_ready(log)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.strip(), 'INPUT_CHAIN_CAPTURE_READY devices=1')

    def test_startup_race_waits_for_ready_record(self):
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp) / 'raw.log'
            log.write_text('INPUT_CAPTURE_DEVICE index=1')
            proc = subprocess.Popen(['/bin/sh', str(CAPTURE_READY), str(log), '5'],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            time.sleep(.2)
            with log.open('a') as stream:
                stream.write(' named=1 monotonic=1 name=QEMU Virtio Keyboard\n'
                             'INPUT_CAPTURE_READY devices=1\n')
            out, err = proc.communicate(timeout=8)
            self.assertEqual(proc.returncode, 0, out + err)
            self.assertEqual(out.strip(), 'INPUT_CHAIN_CAPTURE_READY devices=1')

    def test_missing_file_is_retried_until_collector_creates_it(self):
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp) / 'raw.log'
            proc = subprocess.Popen(['/bin/sh', str(CAPTURE_READY), str(log), '5'],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            time.sleep(.2)
            log.write_text('INPUT_CAPTURE_DEVICE index=1 named=1 monotonic=1 name=Keyboard\n'
                           'INPUT_CAPTURE_READY devices=1\n')
            out, err = proc.communicate(timeout=8)
            self.assertEqual(proc.returncode, 0, out + err)
            self.assertIn('INPUT_CHAIN_CAPTURE_READY devices=1', out)

    def test_missing_ready_times_out(self):
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp) / 'raw.log'
            log.write_text('INPUT_CAPTURE_DEVICE index=1 named=1 monotonic=1 name=QEMU Virtio Keyboard\n')
            result = self.run_ready(log)
            self.assertEqual(result.returncode, 1)
            self.assertIn('INPUT_CHAIN_CAPTURE_READY_ERROR poll_limit_reached', result.stdout)

    def test_nonfile_and_inspector_read_errors_fail_without_retry(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            result = self.run_ready(root, '5')
            self.assertEqual(result.returncode, 1)
            self.assertIn('log_unreadable', result.stdout)
            log = root / 'raw.log'
            log.write_text('not read\n')
            bindir = root / 'bin'
            bindir.mkdir()
            head = bindir / 'head'
            head.write_text('#!/bin/sh\nexit 7\n')
            head.chmod(0o755)
            env = dict(os.environ, PATH=str(bindir) + ':/usr/bin:/bin')
            result = self.run_ready(log, '5', env=env)
            self.assertEqual(result.returncode, 1)
            self.assertIn('log_read_failed', result.stdout)

    def test_sleep_error_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            bindir = root / 'bin'
            bindir.mkdir()
            sleeper = bindir / 'sleep'
            sleeper.write_text('#!/bin/sh\nexit 9\n')
            sleeper.chmod(0o755)
            env = dict(os.environ, PATH=str(bindir) + ':/usr/bin:/bin')
            result = self.run_ready(root / 'absent.log', '5', env=env)
            self.assertEqual(result.returncode, 1)
            self.assertIn('poll_sleep_failed', result.stdout)

    def test_failed_or_malformed_ready_fails_closed(self):
        cases = (
            'INPUT_CAPTURE_ERROR no devices\n',
            'INPUT_CAPTURE_DEVICE index=1 named=1 monotonic=1 name=Keyboard\n'
            'INPUT_CAPTURE_READY devices=2\n',
            'INPUT_CAPTURE_DEVICE index=1 named=1 monotonic=1 name=Keyboard\n'
            'INPUT_CAPTURE_READY devices=x\n',
            'INPUT_CAPTURE_DEVICE index=1 named=1 monotonic=1 name=Keyboard\n'
            'INPUT_CAPTURE_READY devices=1\n'
            'INPUT_CAPTURE_DONE events=0 repeats=0 dropped=0 error=0\n',
        )
        for content in cases:
            with tempfile.TemporaryDirectory() as temp:
                log = Path(temp) / 'raw.log'
                log.write_text(content)
                result = self.run_ready(log, '5')
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn('INPUT_CHAIN_CAPTURE_READY_ERROR', result.stdout)

    def test_atomic_completion_status_blocks_readiness_when_done_is_beyond_prefix(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            log, status = root / 'raw.log', root / 'status'
            log.write_text('INPUT_CAPTURE_DEVICE index=1 named=1 monotonic=1 name=Keyboard\n'
                           'INPUT_CAPTURE_READY devices=1\n')
            status.write_text('0\n')
            result = self.run_ready(log, '1', status)
            self.assertEqual(result.returncode, 1)
            self.assertIn('collector_finished_before_key_send', result.stdout)

    def test_status_created_during_inspection_blocks_ready_output(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            log, status = root / 'raw.log', root / 'status'
            log.write_text('fake inspector supplies readiness\n')
            bindir = root / 'bin'
            bindir.mkdir()
            awk = bindir / 'awk'
            awk.write_text('#!/bin/sh\ntouch "$STATUS_TO_CREATE"\necho "READY 1"\n')
            awk.chmod(0o755)
            env = dict(os.environ, PATH=str(bindir) + ':/usr/bin:/bin',
                       STATUS_TO_CREATE=str(status))
            result = self.run_ready(log, '1', status, env)
            self.assertEqual(result.returncode, 1)
            self.assertNotIn('INPUT_CHAIN_CAPTURE_READY devices=1', result.stdout)
            self.assertIn('collector_finished_before_key_send', result.stdout)

    def test_awk_ready_text_with_nonzero_inspector_status_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            log = root / 'raw.log'
            log.write_text('ignored by the fake inspector\n')
            bindir = root / 'bin'
            bindir.mkdir()
            awk = bindir / 'awk'
            awk.write_text('#!/bin/sh\necho "READY 1"\nexit 7\n')
            awk.chmod(0o755)
            env = dict(os.environ, PATH=str(bindir) + ':/usr/bin:/bin')
            result = self.run_ready(log, '1', env=env)
            self.assertEqual(result.returncode, 1)
            self.assertNotIn('INPUT_CHAIN_CAPTURE_READY devices=1', result.stdout)
            self.assertIn('INPUT_CHAIN_CAPTURE_READY_ERROR inspector_failed', result.stdout)

    def test_hup_int_term_during_inspector_exit_nonzero_no_ready_or_sender_call(self):
        for signum, expected_rc in ((signal.SIGHUP, 129),
                                    (signal.SIGINT, 130),
                                    (signal.SIGTERM, 143)):
            with self.subTest(signal=signum), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                log = root / 'raw.log'
                log.write_text('fixture for delayed inspector\n')
                bindir = root / 'bin'
                bindir.mkdir()
                started = root / 'awk-started'
                awk = bindir / 'awk'
                awk.write_text('#!/bin/sh\ntouch "$AWK_STARTED"\nsleep .5\necho "READY 1"\nexit 0\n')
                awk.chmod(0o755)
                env = dict(os.environ, PATH=str(bindir) + ':/usr/bin:/bin',
                           AWK_STARTED=str(started), TMPDIR=str(root))
                proc = subprocess.Popen(
                    ['/bin/sh', str(CAPTURE_READY), str(log), '5'],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
                try:
                    deadline = time.monotonic() + 5
                    while not started.exists() and time.monotonic() < deadline:
                        time.sleep(.01)
                    self.assertTrue(started.exists(), 'fake inspector did not start')
                    proc.send_signal(signum)
                    out, err = proc.communicate(timeout=5)
                    self.assertEqual(proc.returncode, expected_rc, out + err)
                    self.assertNotIn('INPUT_CHAIN_CAPTURE_READY devices=', out)
                    self.assertEqual(list(root.glob('input-capture-ready.*')), [])
                    sender_calls = []
                    qemu_log = (SenderGuard.BASE + 'INPUT_CHAIN_CAPTURE_STARTED seconds=45\n' + out
                                + '__RC7=%d\n' % expected_rc)
                    with self.assertRaisesRegex(ValueError, 'raw input capture'):
                        READY.guarded_type(
                            qemu_log, lambda: sender_calls.append(True),
                            require_input_capture_ready=True, capture_ready_step=7)
                    self.assertEqual(sender_calls, [])
                finally:
                    if proc.poll() is None:
                        proc.kill()
                        proc.communicate(timeout=2)

    def test_successful_inspector_output_must_be_one_canonical_numeric_ready(self):
        for output in ('READY x\n', 'READY 0\n', 'READY 33\n', 'READY 1\nextra\n'):
            with self.subTest(output=output), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                log = root / 'raw.log'
                log.write_text('ignored by the fake inspector\n')
                bindir = root / 'bin'
                bindir.mkdir()
                awk = bindir / 'awk'
                awk.write_text('#!/bin/sh\nprintf "%s" "$FAKE_AWK_OUTPUT"\nexit 0\n')
                awk.chmod(0o755)
                env = dict(os.environ, PATH=str(bindir) + ':/usr/bin:/bin',
                           FAKE_AWK_OUTPUT=output)
                result = self.run_ready(log, '1', env=env)
                self.assertEqual(result.returncode, 1)
                self.assertNotIn('INPUT_CHAIN_CAPTURE_READY devices=', result.stdout)

    def test_each_inspection_reads_at_most_64k(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            log = root / 'growing.raw'
            log.write_bytes(b'x' * (1024 * 1024))
            bindir = root / 'bin'
            bindir.mkdir()
            awk = bindir / 'awk'
            awk.write_text('#!/bin/sh\nfor file do :; done\nwc -c <"$file" >"$AWK_SIZE_FILE"\necho WAIT\n')
            awk.chmod(0o755)
            sizefile = root / 'size'
            env = dict(os.environ, PATH=str(bindir) + ':/usr/bin:/bin',
                       AWK_SIZE_FILE=str(sizefile))
            result = self.run_ready(log, '1', env=env)
            self.assertEqual(result.returncode, 1)
            self.assertLessEqual(int(sizefile.read_text()), 65536)


class SenderGuard(unittest.TestCase):
    BASE = 'KONSOLE_FOCUSED_TERMINAL_READY\nALPINE_PLASMA_INPUT_READY\n__RC3=0\n'

    def test_sender_not_called_without_successful_unique_ordered_completion(self):
        called = []
        cases = (
            'INPUT_CHAIN_CAPTURE_STARTED seconds=45\n'
            'INPUT_CHAIN_CAPTURE_READY devices=1\n',
            'INPUT_CHAIN_CAPTURE_STARTED seconds=45\n'
            'INPUT_CHAIN_CAPTURE_READY devices=1\n__RC7=1\n',
            'INPUT_CHAIN_CAPTURE_STARTED seconds=45\n'
            'INPUT_CHAIN_CAPTURE_READY devices=1\n__RC7=143\n',
            'INPUT_CHAIN_CAPTURE_STARTED seconds=45\n'
            'INPUT_CHAIN_CAPTURE_READY devices=1\n__RC7=0\n__RC7=0\n',
            'INPUT_CHAIN_CAPTURE_STARTED seconds=45\n'
            '__RC7=0\nINPUT_CHAIN_CAPTURE_READY devices=1\n',
            'INPUT_CHAIN_CAPTURE_STARTED seconds=45\n'
            'INPUT_CHAIN_CAPTURE_READY devices=33\n__RC7=0\n',
            'INPUT_CHAIN_CAPTURE_STARTED seconds=45\n'
            'INPUT_CHAIN_CAPTURE_READY devices=1\nINPUT_CHAIN_CAPTURE_READY devices=1\n__RC7=0\n',
            'INPUT_CHAIN_CAPTURE_STARTED seconds=45\n'
            'INPUT_CHAIN_CAPTURE_READY_ERROR timeout\n__RC7=1\n',
        )
        for suffix in cases:
            with self.subTest(suffix=suffix), self.assertRaisesRegex(ValueError, 'raw input capture'):
                READY.guarded_type(self.BASE + suffix, lambda: called.append(True),
                                   require_input_capture_ready=True, capture_ready_step=7)
        self.assertEqual(called, [])

    def test_sender_called_once_after_valid_ready_and_completed_rc7(self):
        called = []
        text = ('INPUT_CHAIN_CAPTURE_STARTED seconds=45\n'
                'INPUT_CHAIN_CAPTURE_READY devices=2\n__RC7=0\n' + self.BASE)
        result = READY.guarded_type(text, lambda: called.append(True) or 0,
                                    require_input_capture_ready=True, capture_ready_step=7)
        self.assertEqual(result, 0)
        self.assertEqual(called, [True])


class Wiring(unittest.TestCase):
    def test_explicit_flag_is_validated_before_side_effecting_steps(self):
        env = dict(os.environ, ALPINE_INPUT_CHAIN_DIAGNOSTICS='invalid')
        env.pop('ALPINE_QT_KEY_TRACE', None)
        result = subprocess.run(['/bin/sh', str(ROOT / 'scripts/alpine-kms-tests.sh')],
                                cwd=ROOT, env=env, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertIn('ALPINE_INPUT_CHAIN_DIAGNOSTICS must be 0 or 1', result.stderr)

    def test_wrapper_forwards_trace_flags_inside_su_command_only_when_enabled(self):
        with tempfile.TemporaryDirectory() as temp:
            bindir = Path(temp)
            (bindir / 'id').write_text(
                '#!/bin/sh\n'
                'if [ "$1" = -u ] && [ "$#" -eq 1 ]; then echo 0\n'
                'elif [ "$1" = -u ] && [ "$2" = user ]; then echo 1000\n'
                'else exec /usr/bin/id "$@"; fi\n')
            (bindir / 'su').write_text(
                '#!/bin/sh\n'
                'while [ "$#" -gt 0 ]; do\n'
                '  if [ "$1" = -c ]; then shift; exec /usr/bin/env -i PATH="$PATH" /bin/sh -c "$1"; fi\n'
                '  shift\n'
                'done\nexit 2\n')
            (bindir / 'alpine-plasma-probe').write_text(
                '#!/bin/sh\n'
                'printf "TRACE_FLAGS=%s,%s,%s,SEAT=%s\\n" '
                '"${ALPINE_PLASMA_KEY_TRACE-unset}" "${ALPINE_KONSOLE_INPUT_TRACE-unset}" '
                '"${ALPINE_QT_KEY_TRACE-unset}" "${ALPINE_PLASMA_EXTERNAL_SEAT-unset}"\n')
            for name in ('id', 'su', 'alpine-plasma-probe'):
                (bindir / name).chmod(0o755)

            for diagnostic, expected in (
                    (None, 'TRACE_FLAGS=unset,unset,unset,SEAT=1'),
                    ('1', 'TRACE_FLAGS=1,1,1,SEAT=1')):
                env = dict(os.environ, PATH=str(bindir) + ':/usr/bin:/bin',
                           ALPINE_PLASMA_OUTPUT='headless')
                for name in ('ALPINE_INPUT_CHAIN_DIAGNOSTICS', 'ALPINE_PLASMA_KEY_TRACE',
                             'ALPINE_KONSOLE_INPUT_TRACE', 'ALPINE_QT_KEY_TRACE'):
                    env.pop(name, None)
                if diagnostic is not None:
                    env['ALPINE_INPUT_CHAIN_DIAGNOSTICS'] = diagnostic
                result = subprocess.run(
                    ['/bin/sh', str(ROOT / 'userland/alpine/desktop-overlay/usr/bin/alpine-plasma-user-probe')],
                    cwd=ROOT, env=env, capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(expected, result.stdout)

    def test_sequence_deadlines_and_capture_order_remain_fixed(self):
        source = (ROOT / 'scripts/alpine-kms-tests.sh').read_text()
        self.assertIn('--boot-timeout 300 --timeout 780', source)
        self.assertIn('seq 700', source)
        self.assertIn('seq 20', source)
        self.assertIn('seq 300', source)
        self.assertIn('seq 150', source)
        self.assertIn("tty > /tmp/kms-tty; id -u > /tmp/kms-uid; echo kms-input-ok > /tmp/kms-input-ok.tmp; mv /tmp/kms-input-ok.tmp /tmp/kms-input-ok\\n", source)
        self.assertIn('--ready-step 3', source)
        self.assertIn('ALPINE_INPUT_CHAIN_DIAGNOSTICS:-0', source)
        self.assertLess(source.index('grep -q "^violations 0" /proc/lockdep'), source.index('INPUT_CHAIN_RAW_CAPTURE_BEGIN'))
        self.assertIn('if [ "$test_rc" -ne 0 ];then exit "$test_rc";fi', source)
        self.assertIn('run_id=$(date +%Y%m%d%H%M%S)-$$', source)
        self.assertIn('input-capture-ready /tmp/kms-input-chain.raw.log 10 /tmp/kms-input-chain.raw.status', source)
        self.assertIn('capture_ready_step=$#', source)
        self.assertIn('--capture-ready-step $capture_ready_step', source)
        self.assertIn('--require-input-capture-ready', source)
        self.assertIn('input-capture-ready', source)

    def test_full_harness_default_argv_parity_with_mocked_qemu_test(self):
        with tempfile.TemporaryDirectory() as temp:
            tree = Path(temp)
            (tree / 'scripts').mkdir()
            (tree / 'userland/alpine-desktop-root-x86_64/usr/bin').mkdir(parents=True)
            for name in ('alpine-kms-tests.sh', 'input-chain-check.py', 'input-capture-check.py',
                         'inner-keyboard-dispatch-check.py', 'qt-key-observer-check.py',
                         'qemu-type.py'):
                shutil.copy2(ROOT / 'scripts' / name, tree / 'scripts' / name)
            mkdisk = tree / 'scripts/mkdisk.sh'
            mkdisk.write_text('#!/bin/sh\nprintf "%s\\n" "$@" >"$MKDISK_CAPTURE"\n')
            mkdisk.chmod(0o755)
            qemu_test = tree / 'scripts/qemu-test.py'
            qemu_test.write_text(
                '#!/usr/bin/env python3\n'
                'import json, os, sys, time\n'
                'from pathlib import Path\n'
                'if "--log" in sys.argv:\n'
                ' p=Path(sys.argv[sys.argv.index("--log")+1]); p.parent.mkdir(parents=True, exist_ok=True); p.write_text("")\n'
                'i=sys.argv.index("--log"); commands=sys.argv[i+2:]; events=[]; screenshot_done=False\n'
                'if os.environ.get("MOCK_SCREENSHOT_FAIL") == "1":\n'
                ' for command in commands:\n'
                '  if command.startswith("host:python3 scripts/qemu-screenshot.py") and "-before.png" in command:\n'
                '   time.sleep(.05); events.append("pre_screenshot_delayed_failed"); screenshot_done=True; break\n'
                '  if "INPUT_CHAIN_CAPTURE_STARTED seconds=45" in command:\n'
                '   events.append("capture_after_screenshot" if screenshot_done else "capture_before_screenshot")\n'
                'json.dump({"argv": sys.argv[1:], "qemu_extra": os.environ.get("QEMU_EXTRA"), "events": events}, '
                'open(os.environ["QEMU_CAPTURE"], "w"))\n'
                'raise SystemExit(int(os.environ["MOCK_QEMU_RC"]))\n')
            qemu_test.chmod(0o755)
            for helper in ('desktopframecheck', 'desktopa11y', 'desktopa11y-dbus'):
                path = tree / 'userland/alpine-desktop-root-x86_64/usr/bin' / helper
                path.write_text('#!/bin/sh\nexit 0\n')
                path.chmod(0o755)
            for helper in ('input-capture-session', 'inputeventcapture', 'input-capture-ready',
                           'konsole-key-trace', 'wayland-presentation-proxy'):
                path = tree / 'userland/alpine-desktop-root-x86_64/usr/bin' / helper
                path.write_text('#!/bin/sh\nexit 0\n')
                path.chmod(0o755)
            libdir = tree / 'userland/alpine-desktop-root-x86_64/usr/lib'
            libdir.mkdir(parents=True)
            (libdir / 'lib9os-qt-key-observer.so').write_text('mock fixture only\n')

            def run(diagnostic, qemu_rc, screenshot_fail=False):
                capture = tree / 'qemu.json'
                mkdisk_capture = tree / 'mkdisk.txt'
                env = dict(os.environ, PATH='/usr/bin:/bin',
                           QEMU_CAPTURE=str(capture), MKDISK_CAPTURE=str(mkdisk_capture),
                           MOCK_QEMU_RC=str(qemu_rc), QEMU_EXTRA='-accel tcg',
                           MOCK_SCREENSHOT_FAIL='1' if screenshot_fail else '0',
                           ALPINE_QT_KEY_TRACE='0', ALPINE_A11Y_DBUS_VALIDATE='0',
                           ALPINE_TERMINAL_QUERY='atspi')
                env.pop('ALPINE_INPUT_CHAIN_DIAGNOSTICS', None)
                if diagnostic is not None:
                    env['ALPINE_INPUT_CHAIN_DIAGNOSTICS'] = diagnostic
                result = subprocess.run(['/bin/sh', 'scripts/alpine-kms-tests.sh', 'x86_64'],
                                        cwd=tree, env=env, capture_output=True, text=True, timeout=10)
                captured = json.loads(capture.read_text())
                mkdisk_args = mkdisk_capture.read_text().splitlines()
                def normalize(value):
                    value = re.sub(r'/tmp/9os-kms-[^/]+/', '/tmp/9os-kms-<run>/', value)
                    value = re.sub(r'test-alpine-kms-x86_64-[0-9]{14}-[0-9]+', 'test-alpine-kms-x86_64-<run>', value)
                    return re.sub(r'/build/9os-kms-output-[^/]+/', '/build/9os-kms-output-<run>/', value)
                captured['argv'] = [normalize(value) for value in captured['argv']]
                captured['qemu_extra'] = normalize(captured['qemu_extra'])
                return result, captured, mkdisk_args

            for qemu_rc in (0, 17):
                unset_result, unset_args, unset_disk = run(None, qemu_rc)
                zero_result, zero_args, zero_disk = run('0', qemu_rc)
                with self.subTest(qemu_rc=qemu_rc):
                    self.assertEqual(unset_args, zero_args)
                    self.assertEqual(unset_disk, zero_disk)
                    self.assertEqual(re.sub(r'9os-kms-output-[^\s/]+', '9os-kms-output-<run>', unset_result.stdout),
                                     re.sub(r'9os-kms-output-[^\s/]+', '9os-kms-output-<run>', zero_result.stdout))
                    self.assertEqual(unset_result.stderr, zero_result.stderr)
                    self.assertEqual(unset_result.returncode, qemu_rc)
                    self.assertEqual(zero_result.returncode, qemu_rc)
                    sender = [arg for arg in unset_args['argv']
                              if arg.startswith('host:python3 scripts/qemu-ready-type.py')]
                    self.assertEqual(len(sender), 1)
                    self.assertNotIn('--require-input-capture-ready', sender[0])

            result, diag_args, _ = run('1', 17, screenshot_fail=True)
            with self.subTest(diagnostic_mode=True):
                self.assertEqual(result.returncode, 17, result.stdout + result.stderr)
                sender = [arg for arg in diag_args['argv']
                          if arg.startswith('host:python3 scripts/qemu-ready-type.py')]
                self.assertEqual(len(sender), 1)
                self.assertIn('--require-input-capture-ready', sender[0])
                commands = diag_args['argv'][diag_args['argv'].index('--log') + 2:]
                readiness_step = commands.index(
                    'input-capture-ready /tmp/kms-input-chain.raw.log 10 /tmp/kms-input-chain.raw.status')
                step_arg = re.search(r'--capture-ready-step ([0-9]+)', sender[0])
                self.assertIsNotNone(step_arg)
                self.assertEqual(int(step_arg.group(1)), readiness_step)
                screenshot_step = next(i for i, command in enumerate(commands)
                                       if command.startswith('host:python3 scripts/qemu-screenshot.py')
                                       and '-before.png' in command)
                capture_step = next(i for i, command in enumerate(commands)
                                    if 'INPUT_CHAIN_CAPTURE_STARTED seconds=45' in command)
                self.assertLess(screenshot_step, capture_step)
                self.assertLess(capture_step, readiness_step)
                self.assertLess(readiness_step, commands.index(sender[0]))
                self.assertEqual(diag_args['events'],
                                 ['pre_screenshot_delayed_failed'])
                logarg = diag_args['argv'][diag_args['argv'].index('--log') + 1]
                self.assertIn('INPUT_CHAIN_CHECK_RC=1',
                              (tree / logarg).read_text())


if __name__ == '__main__':
    unittest.main()
