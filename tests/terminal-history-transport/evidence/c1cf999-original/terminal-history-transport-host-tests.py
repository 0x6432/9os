#!/usr/bin/env python3
"""Production-loop/source-bound strict-parser controls; synthetic, NOT guest proof."""
from pathlib import Path
import importlib.util
import re
import unittest

ROOT=Path(__file__).resolve().parents[1]
SPEC=importlib.util.spec_from_file_location('history_controls', ROOT/'scripts/terminal-attempt-history-host-tests.py')
H=importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(H)
CONTROLLER=(ROOT/'scripts/qemu-test.py').read_text()
# Bind this tiny predicate control to the unchanged production expression. This
# is not a controller exemption and does not execute qemu-test.py or start make.
STRICT="passed = bool(m) and m.group(1) == b'0' and not re.search(rb'\\bFAIL', body) and not state['panic']"
assert STRICT in CONTROLLER
COLLISION=(b'DBUS_A11Y_QUERY method=GetRole phase=begin bus=:1.2 '
           b'path=/org/a11y/atspi/accessible/FAIL mono_ns=1\n')

def strict_accept(body, rc=0, panic=False):
    return rc==0 and re.search(rb'\bFAIL', body) is None and not panic

def collision_run(functions=H.FUNCTIONS):
    return H.execute([(1, COLLISION.decode()+H.TERMINAL+H.WITNESS+H.FINAL),
                      (0, H.TERMINAL.replace('focused=0','focused=1')+
                       H.WITNESS.replace('qualified=0','qualified=1')+
                       H.FINAL.replace('models=0','models=1').replace('NOT_OBSERVED','OBSERVED'))],
                     functions=functions)

class Transport(unittest.TestCase):
    def test_original_history_collision_preserved_before_fix(self):
        r, slots, calls=collision_run()
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(len(calls),2)
        self.assertIn(COLLISION.decode().strip(), slots['first'])
        self.assertIn('attempt=1 rc=1', slots['first'])
        self.assertIn('attempt=2 rc=0', slots['last'])
        self.assertIn(COLLISION, r.stdout.encode())
        self.assertFalse(strict_accept(r.stdout.encode(), r.returncode))

    def test_strict_parser_controls_remain_strict(self):
        self.assertTrue(strict_accept(b'normal\n'))
        for body,rc,panic in [(b'normal\n',1,False),(b'normal\n',0,True),
                              (b'FAIL\n',0,False),(b'/FAILURE\n',0,False),
                              (b' FAILED\n',0,False)]:
            self.assertFalse(strict_accept(body,rc,panic))

if __name__=='__main__': unittest.main()
