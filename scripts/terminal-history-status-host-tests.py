#!/usr/bin/env python3
"""Checked history transport controls; native shell mocks, NOT guest/cold proof.

Exercises the real OR-list caller. Historical unfixed F1 evidence is separate.
"""
from pathlib import Path
import hashlib
import importlib.util
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
# Overrides emitter-only execution; production-loop helpers retain their own shell.
EMITTER_SHELL = shlex.split(os.environ.get('HISTORY_TEST_SHELL', '/bin/sh'))
if not EMITTER_SHELL:
    raise ValueError('HISTORY_TEST_SHELL must name an executable')


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


H = load('history_status_loop', 'terminal-attempt-history-host-tests.py')
D = load('history_status_decoder', 'terminal-history-decode.py')
STRICT = "passed = bool(m) and m.group(1) == b'0' and not re.search(rb'\\bFAIL', body) and not state['panic']"
assert STRICT in (ROOT / 'scripts/qemu-test.py').read_text()
PAYLOAD = b'FAIL\x00\r\n' + bytes(range(256))


def emit(payload, override='', functions=H.FUNCTIONS):
    with tempfile.TemporaryDirectory(prefix='history-status-') as tmp:
        root = Path(tmp)
        (root / 'konsole-attempt-first').write_bytes(payload)
        # Keep the exact production invocation, separately observing the emitter
        # status through a wrapper; the production caller must still return 0.
        observed = functions.replace('emit_terminal_attempts() {', 'status_inner_emitter() {', 1)
        script = observed + '\n' + override + '''
query_attempt=2; query_recording_failed=0
emit_terminal_attempts() {
 if status_inner_emitter; then status_rc=0; else status_rc=$?; fi
 printf '%s' "$status_rc" > "$D/emitter-status"
 return "$status_rc"
}
emit_terminal_attempts || true
'''
        result = subprocess.run(EMITTER_SHELL + ['-eu', '-c', script], capture_output=True,
                                timeout=5, env={**os.environ, 'D': tmp})
        status = (root / 'emitter-status').read_text()
        files = sorted(p.name for p in root.iterdir())
        return result, status, files


def faulty(utility, mode, status):
    if mode == 'zero':
        output = ':'
    elif mode == 'partial':
        output = "printf '46'" if utility != 'wc' else "printf '1'"
    else:
        output = f'command {utility} "$@"'
    return f'{utility}() {{ {output}; return {status}; }}\n'


class Status(unittest.TestCase):
    def assert_unknown(self, result, emitter_status, stage):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(emitter_status, '1')
        self.assertIn(f'KONSOLE_QUERY_TRANSPORT_UNKNOWN slot=first stage={stage}\n'.encode(), result.stdout)
        self.assertNotIn(b'KONSOLE_QUERY_HISTORY_END', result.stdout)
        self.assertNotRegex(result.stdout + result.stderr, rb'\bFAIL')
        with self.assertRaises(ValueError):
            D.decode_history(result.stdout)

    def test_nonzero_utilities_zero_partial_and_complete_bytes(self):
        for utility, status in [('wc', 5), ('od', 7), ('tr', 9)]:
            for mode in ('zero', 'partial', 'complete'):
                with self.subTest(utility=utility, mode=mode):
                    result, rc, files = emit(PAYLOAD, faulty(utility, mode, status))
                    self.assert_unknown(result, rc, utility)
                    self.assertEqual(files, ['emitter-status', 'konsole-attempt-first'])

    def test_empty_input_still_checks_each_utility_status(self):
        for utility, status in [('wc', 5), ('od', 7), ('tr', 9)]:
            with self.subTest(utility=utility):
                result, rc, _ = emit(b'', faulty(utility, 'complete', status))
                self.assert_unknown(result, rc, utility)

    def test_successful_but_malformed_or_short_hex_is_unknown(self):
        for utility in ('od', 'tr'):
            for value in ('46', 'gg', 'AA', '464', ''):
                with self.subTest(utility=utility, value=value):
                    result, rc, _ = emit(PAYLOAD, f"{utility}() {{ printf '%s' '{value}'; return 0; }}\n")
                    self.assert_unknown(result, rc, 'hex')

    def test_successful_wc_malformed_noncanonical_or_oversized_count(self):
        # Match byte lengths where possible, so canonicality is not accidentally
        # tested only through the independent hex-length mismatch check.
        for value, payload in [('00', b''), ('080', b'x' * 80),
                               ('0001', b'x'), ('notdecimal', PAYLOAD),
                               ('5121', b'x' * 5121), ('99999', b''),
                               ('', b''), ('-1', b''), ('1 2', b'')]:
            with self.subTest(value=value):
                result, rc, _ = emit(payload, f"wc() {{ printf '%s' '{value}'; return 0; }}\n")
                self.assert_unknown(result, rc, 'hex')

    def test_normal_binary_and_empty_roundtrip(self):
        for payload in (b'', PAYLOAD, bytes(range(256)) * 20):
            with self.subTest(size=len(payload)):
                result, rc, files = emit(payload)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(rc, '0')
                self.assertEqual(D.decode_history(result.stdout), {'first': payload})
                self.assertNotRegex(result.stdout, rb'\bFAIL')
                self.assertLess(len(result.stdout), 128 * 1024)
                self.assertEqual(files, ['emitter-status', 'konsole-attempt-first'])

    def test_production_loop_status_failure_preserves_original_outcomes(self):
        for utility, status in [('wc', 5), ('od', 7), ('tr', 9)]:
            for success in (False, True):
                with self.subTest(utility=utility, success=success):
                    attempts = [(1, H.TERMINAL + H.WITNESS + H.FINAL)]
                    if success:
                        attempts.append((0, H.FINAL.replace('NOT_OBSERVED', 'OBSERVED')))
                    result, slots, calls = H.execute(attempts, override=faulty(utility, 'complete', status))
                    self.assertEqual(result.returncode, 0 if success else 1, result.stderr)
                    self.assertEqual(len(calls), len(attempts))
                    self.assertIn('KONSOLE_QUERY_TRANSPORT_UNKNOWN slot=first stage=' + utility, result.stdout)
                    self.assertNotIn('KONSOLE_QUERY_HISTORY_END', result.stdout)
                    self.assertIn('attempt=1 rc=1', slots['first'])
                    self.assertIn('RESULT pty=1 terminal=' + str(int(success)), result.stdout)

    def test_final_raw_sentinel_remains_authoritative(self):
        text = 'final original /FAIL\n' + H.FINAL.replace('NOT_OBSERVED', 'OBSERVED')
        for override in ('', faulty('od', 'complete', 7)):
            with self.subTest(override=override):
                result, _, calls = H.execute([(0, text)], override=override)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(len(calls), 1)
                self.assertRegex(result.stdout.encode(), rb'\bFAIL')
                # The unchanged strict controller rejects this body despite rc0.
                self.assertFalse(result.returncode == 0 and re.search(rb'\bFAIL', result.stdout.encode()) is None)

    def test_independent_od_tr_status_guard_mutations_are_semantically_killed(self):
        # Locate the checked utility command substitution, then neutralize only
        # its status. This removes the guard's effect without breaking syntax or
        # changing emitted bytes. A surviving mutant must fail this test.
        for utility, status in [('od', 7), ('tr', 9)]:
            with self.subTest(utility=utility):
                pattern = re.compile(r'\$\(([^\n]*?LC_ALL=C ' + utility + r'[^\n]*?)\)')
                matches = list(pattern.finditer(H.FUNCTIONS))
                self.assertEqual(len(matches), 1, 'expected one checked utility substitution for ' + utility)
                match = matches[0]
                mutant = H.FUNCTIONS[:match.start()] + '$(' + match[1] + '; :)' + H.FUNCTIONS[match.end():]
                good, good_rc, _ = emit(PAYLOAD, faulty(utility, 'complete', status))
                self.assert_unknown(good, good_rc, utility)
                result, rc, _ = emit(PAYLOAD, faulty(utility, 'complete', status), functions=mutant)
                # Prove this is the specific semantic survivor, not shell error.
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, b'')
                self.assertEqual(rc, '0')
                self.assertEqual(D.decode_history(result.stdout), {'first': PAYLOAD})
                with self.assertRaises(AssertionError):
                    self.assert_unknown(result, rc, utility)
                print('SEMANTIC MUTANT REJECTED:', utility, hashlib.sha256(mutant.encode()).hexdigest())


if __name__ == '__main__':
    print('production_source_sha256=' + hashlib.sha256(H.TEXT.encode()).hexdigest(), flush=True)
    unittest.main(verbosity=2)
