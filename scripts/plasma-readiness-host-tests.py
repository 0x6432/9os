#!/usr/bin/env python3
"""Actual shell marker checks + fail-closed sender tests, no GUI/keyboard proof."""
import importlib.util
import inspect
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('ready_type', ROOT/'scripts/qemu-ready-type.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
SCRIPT = (ROOT/'scripts/alpine-kms-tests.sh').read_text()
COMMANDS = [shlex.split(line.strip().removesuffix('\\').strip())[0]
            for line in SCRIPT.splitlines() if line.lstrip().startswith("'for i in $(seq")]
TERMINAL = next(c for c in COMMANDS if c.startswith('for i in $(seq 700)'))
DOLPHIN = next(c for c in COMMANDS if c.startswith('for i in $(seq 300)'))
GOOD = 'KONSOLE_FOCUSED_TERMINAL_READY\nALPINE_PLASMA_INPUT_READY\n'
PS = ' 1417 34 R grep -q ALPINE_PLASMA_INPUT_READY /tmp/plasma-kms.log\n'
PATTERN = '^DOLPHIN_MODEL_READY uid=1000 service=[^ ]+ object=/[^ ]*$'


def shell(command, log):
    with tempfile.TemporaryDirectory(prefix='9os-readiness-') as t:
        d = Path(t)
        (d/'log').write_text(log)
        (d/'pid').write_text(str(os.getpid()))
        (d/'seq').write_text('#!/bin/sh\necho 1\n')
        (d/'sleep').write_text('#!/bin/sh\nexit 0\n')
        (d/'seq').chmod(0o755)
        (d/'sleep').chmod(0o755)
        command = command.replace('/tmp/plasma-kms.log', str(d/'log')).replace('/tmp/plasma-kms.pid', str(d/'pid'))
        return subprocess.run(['sh', '-c', command], capture_output=True, text=True,
                              env=dict(os.environ, PATH=t+':'+os.environ['PATH']), timeout=5)


class Witness(unittest.TestCase):
    def test_real_terminal_lines(self):
        result = shell(TERMINAL, GOOD)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, GOOD)

    def test_failure_ps_reproduces_old_false_positive(self):
        old = TERMINAL.replace('grep -qx ALPINE_PLASMA_INPUT_READY', 'grep -q ALPINE_PLASMA_INPUT_READY', 1)
        old = old.replace('grep -x KONSOLE_FOCUSED_TERMINAL_READY /tmp/plasma-kms.log && grep -x ALPINE_PLASMA_INPUT_READY', 'grep -q ALPINE_PLASMA_INPUT_READY', 1)
        self.assertNotEqual(old, TERMINAL)
        self.assertEqual(shell(old, PS).returncode, 0)
        self.assertNotEqual(shell(TERMINAL, PS).returncode, 0)

    def test_terminal_shell_rejects_missing_or_embedded(self):
        for log in ['', PS, 'ALPINE_PLASMA_INPUT_READY\n', 'KONSOLE_FOCUSED_TERMINAL_READY\n',
                    'echo KONSOLE_FOCUSED_TERMINAL_READY\necho ALPINE_PLASMA_INPUT_READY\n',
                    'prefix '+GOOD.replace('\n', ' suffix\n')]:
            with self.subTest(log=log):
                self.assertNotEqual(shell(TERMINAL, log).returncode, 0)

    def test_dolphin_uid_and_complete_line(self):
        good = 'DOLPHIN_MODEL_READY uid=1000 service=org.kde.dolphin-23 object=/dolphin/Dolphin_1\n'
        self.assertEqual(shell(DOLPHIN, good).returncode, 0)
        for bad in ['', '42 R grep -q DOLPHIN_MODEL_READY\n', 'prefix '+good,
                    good.replace('1000','0'), good.replace('service=org.kde.dolphin-23', 'service='),
                    good.replace(' object=/', ' object=invalid'), good.rstrip()+' extra\n']:
            with self.subTest(log=bad):
                self.assertNotEqual(shell(DOLPHIN, bad).returncode, 0)

    def test_linux_oracle_same_anchored_contract(self):
        oracle = (ROOT/'scripts/alpine-linux-oracle.sh').read_text()
        self.assertEqual(oracle.count('grep -Eq "'+PATTERN+'"'), 2)
        self.assertNotIn('grep -q DOLPHIN_MODEL_READY', oracle)

    def test_guard_positive_and_crlf(self):
        for good in [GOOD+'__RC3=0\n', (GOOD+'__RC3=0\n').replace('\n','\r\n')]:
            sent = []
            self.assertEqual(m.guarded_type(good, lambda: sent.append('one') or 23), 23)
            self.assertEqual(sent, ['one'])

    def test_guard_never_calls_sender_on_bad_witnesses(self):
        good = GOOD+'__RC3=0\n'
        bad = ['', '__RC3=0\n', PS+'__RC3=0\n', good.replace('__RC3=0','__RC3=1'),
               good.replace('__RC3=0','__RC4=0'), good+'__RC3=0\n', good+'__RC3=1\n',
               good+'ALPINE_PLASMA_INPUT_READY\n', good.replace('KONSOLE_FOCUSED_TERMINAL_READY\n',''),
               good.replace('ALPINE_PLASMA_INPUT_READY\n',''),
               'echo KONSOLE_FOCUSED_TERMINAL_READY\necho ALPINE_PLASMA_INPUT_READY\n__RC3=0\n']
        bad += ['__RC3=0\n'+GOOD, 'ALPINE_PLASMA_INPUT_READY\nKONSOLE_FOCUSED_TERMINAL_READY\n__RC3=0\n']
        bad += [good+x+'\n' for x in ['KONSOLE_READINESS pty=1 terminal=0 pid=934',
                   'alpine-plasma: FAIL input readiness', 'alpine-dolphin: FAIL URL', 'ALPINE_PLASMA_FAILURE_PNG_BEGIN']]
        for text in bad:
            with self.subTest(text=text):
                sent = []
                with self.assertRaises(ValueError):
                    m.guarded_type(text, lambda: sent.append('UNSAFE'))
                self.assertEqual(sent, [])

    def test_guard_marker_mutation_is_detected(self):
        source = inspect.getsource(m.ready)
        mutated = source.replace('if lines.count(marker) != 1:', 'if False:')
        start = mutated.index('    terminal = lines.index(')
        stop = mutated.index('    return not any(', start)
        mutated = mutated[:start] + mutated[stop:]
        self.assertNotEqual(source, mutated)
        scope = {'re': re}
        exec(mutated, scope)
        self.assertTrue(scope['ready'](PS+'__RC3=0\n'))
        self.assertFalse(m.ready(PS+'__RC3=0\n'))

    def test_driver_only_uses_guarded_keys(self):
        self.assertIn('scripts/qemu-ready-type.py --log $L --ready-step 3 --qmp $Q', SCRIPT)
        self.assertNotIn('host:python3 scripts/qemu-type.py ', SCRIPT)
        self.assertEqual(SCRIPT.count('grep -Eq "'+PATTERN+'"'), 3)


if __name__ == '__main__':
    unittest.main()
