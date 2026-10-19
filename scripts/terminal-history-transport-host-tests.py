#!/usr/bin/env python3
"""Production-loop/source-bound strict-parser controls; synthetic, NOT guest proof."""
from pathlib import Path
import importlib.util
import hashlib
import os
import subprocess
import tempfile
import re
import unittest

ROOT=Path(__file__).resolve().parents[1]
SPEC=importlib.util.spec_from_file_location('history_controls', ROOT/'scripts/terminal-attempt-history-host-tests.py')
H=importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(H)
DSPEC=importlib.util.spec_from_file_location('history_decode',ROOT/'scripts/terminal-history-decode.py')
D=importlib.util.module_from_spec(DSPEC)
DSPEC.loader.exec_module(D)
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
                       H.WITNESS.replace('focused_states=0','focused_states=1').replace('qualified=0','qualified=1').replace('protocol_error=1','protocol_error=0')+
                       H.FINAL.replace('models=0','models=1').replace('NOT_OBSERVED','OBSERVED'))],
                     functions=functions)

def emit_slots(payloads, functions=H.FUNCTIONS):
    with tempfile.TemporaryDirectory() as temp:
        root=Path(temp)
        for slot,payload in payloads.items():
            (root/('konsole-attempt-'+slot)).write_bytes(payload)
        script=functions+'\nquery_attempt=41;query_recording_failed=0\nemit_terminal_attempts\n'
        return subprocess.run(['/bin/sh','-eu','-c',script],capture_output=True,timeout=5,
                              env={**os.environ,'D':temp})


def record_bytes(payload):
    with tempfile.TemporaryDirectory() as temp:
        root=Path(temp)
        (root/'konsole-focus').write_bytes(payload)
        script=H.FUNCTIONS+'\nclient=777;query_attempt=0;query_recording_failed=0;query_rc=1\nrecord_terminal_attempt\nemit_terminal_attempts\n'
        r=subprocess.run(['/bin/sh','-eu','-c',script],capture_output=True,timeout=5,
                         env={**os.environ,'D':temp,'ALPINE_TERMINAL_QUERY':'live-dbus'})
        slots={p.name.removeprefix('konsole-attempt-'):p.read_bytes() for p in root.glob('konsole-attempt-*')}
        return r,slots


class Transport(unittest.TestCase):
    def test_history_collision_repaired_without_losing_original_bytes(self):
        r, slots, calls=collision_run()
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(len(calls),2)
        self.assertIn(COLLISION.decode().strip(), slots['first'])
        self.assertIn('attempt=1 rc=1', slots['first'])
        self.assertIn('attempt=2 rc=0', slots['last'])
        wire=r.stdout.encode()
        self.assertNotIn(COLLISION, wire)
        self.assertTrue(strict_accept(wire,r.returncode))
        self.assertEqual(D.decode_history(wire),{k:v.encode() for k,v in slots.items() if k!='meta'})

    def test_preserved_original_failure_evidence_stays_a_failure(self):
        evidence=ROOT/'tests/terminal-history-transport/evidence'
        original=(evidence/'unfixed-production-loop.stdout').read_bytes()
        self.assertIn(COLLISION,original)
        self.assertFalse(strict_accept(original))
        self.assertIn(b'RESULT pty=1 terminal=1',original)
        self.assertIn('shell_rc=0',(evidence/'unfixed-production-loop-result.txt').read_text())

    def test_binary_control_chars_and_all_twelve_full_slots_round_trip(self):
        payload=(bytes(range(256))*20)  #5120 bytes, including invalid UTF8/NUL/CR/LF
        slots={slot:payload for slot in D.SLOTS}
        r=emit_slots(slots)
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(D.decode_history(r.stdout),slots)
        self.assertLess(len(r.stdout),128*1024)
        self.assertLessEqual(sum(len(p) for p in slots.values()),62464)
        self.assertTrue(strict_accept(r.stdout))
        for line in r.stdout.splitlines():
            if not line.startswith(b'KONSOLE_'):
                self.assertRegex(line,rb'^[0-9a-f]*$')

    def test_actual_recorder_preserves_binary_prefix_suffix_then_round_trip(self):
        payload=bytes(range(256))*30+b'FAIL\x00\r\n\x1b[31mFAILURE\x1b[0m\n'
        r,slots=record_bytes(payload)
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(D.decode_history(r.stdout),{k:v for k,v in slots.items() if k!='meta'})
        for slot,value in slots.items():
            if slot!='meta':
                self.assertTrue(value.endswith(payload[:2048]+payload[-2048:]))
            self.assertLessEqual(len(value),1024 if slot=='meta' else 5120)
        self.assertLessEqual(sum(map(len,slots.values())),62464)
        self.assertTrue(strict_accept(r.stdout))

    def test_historical_arbitrary_text_does_not_interfere_with_acceptance(self):
        for text in [b'FAIL FAILED FAILURE /FAIL\n',
                     b'__RC0=9\n__RC1=0\nKONSOLE_FOCUSED_TERMINAL_READY\n',
                     b'kernel panic\nOops\npage fault in kernel\n',
                     b'\x1b[31mFAIL\r\b\x1b[0m\x00'+bytes(range(128,256))]:
            r=emit_slots({'first':text,'last':b'original last retained bytes'})
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertEqual(D.decode_history(r.stdout)['first'],text)
            self.assertTrue(strict_accept(r.stdout))
            self.assertIsNone(re.search(rb'(?i)kernel panic|\bpanic:|page fault in kernel|\boops\b|unhandled exception|double fault',r.stdout))
            self.assertIsNone(re.search(rb'__RC[0-9]+=',r.stdout))

    def test_final_original_raw_replay_is_not_claimed_safe_or_changed(self):
        # The inherited final cat is deliberately outside transport protection.
        text=COLLISION.decode()+H.FINAL.replace('NOT_OBSERVED','OBSERVED')
        r,slots,calls=H.execute([(0,text)])
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(len(calls),1)
        self.assertEqual(D.decode_history(r.stdout.encode())['last'],slots['last'].encode())
        self.assertIn(COLLISION,r.stdout.encode())
        self.assertFalse(strict_accept(r.stdout.encode(),r.returncode))

    def test_original_failure_status_is_never_rescued(self):
        for rc in [1,2,124,142,143]:
            r,_,calls=H.execute([(rc,COLLISION.decode())])
            self.assertEqual(r.returncode,1,r.stderr)
            self.assertEqual(len(calls),1)
            self.assertFalse(strict_accept(r.stdout.encode(),r.returncode))
            self.assertIn('attempt=1 rc='+str(rc),D.decode_history(r.stdout.encode())['last'].decode())

    def test_transport_command_failure_has_no_raw_fallback_or_extra_query(self):
        for override in ['od() { return 1; }','tr() { return 1; }','wc() { return 1; }']:
            attempts=[(1,COLLISION.decode()),(0,H.FINAL.replace('NOT_OBSERVED','OBSERVED'))]
            r,_,calls=H.execute(attempts,override=override)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertEqual(len(calls),2)
            self.assertNotIn(COLLISION,r.stdout.encode())
            with self.assertRaises(ValueError): D.decode_history(r.stdout.encode())

    def test_decoder_rejects_damage_duplicates_and_unbounded_records(self):
        r=emit_slots({'first':b'FAIL\x00\r\n'})
        self.assertEqual(r.returncode,0,r.stderr)
        wire=r.stdout
        first_payload=b'4641494c000d0a'
        self.assertIn(first_payload,wire)
        cases=[wire.replace(first_payload,first_payload.upper()),
               wire.replace(first_payload,first_payload[:-1]),
               wire.replace(first_payload,b'gg'),
               wire.replace(b'bytes=7',b'bytes=6'),
               wire.replace(b'slot=first',b'slot=unknown'),
               wire.replace(b'ATTEMPT_END slot=first',b'ATTEMPT_END slot=last'),
               wire.replace(b'KONSOLE_QUERY_HISTORY_END',b''),wire+wire,
               wire.replace(first_payload,b'0'*(128*1024)),
               wire.replace(b'bytes=7',b'bytes=5121')]
        for bad in cases:
            with self.assertRaises(ValueError): D.decode_history(bad)
        # A valid second record cannot repeat/reorder an earlier fixed slot.
        two=emit_slots({'first':b'a','last':b'b'}).stdout
        with self.assertRaises(ValueError): D.decode_history(two.replace(b'slot=last',b'slot=first'))

    def test_decoder_cli_is_binary_exact_and_fails_closed_for_absence(self):
        payload=bytes(range(256))+b'FAIL\x00\r\n'
        wire=emit_slots({'first':payload}).stdout
        with tempfile.TemporaryDirectory() as temp:
            log=Path(temp)/'history.log';log.write_bytes(wire)
            command=['python3',str(ROOT/'scripts/terminal-history-decode.py'),str(log),'--slot']
            r=subprocess.run(command+['first'],capture_output=True,timeout=5)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertEqual(r.stdout,payload)
            r=subprocess.run(command+['last'],capture_output=True,timeout=5)
            self.assertEqual(r.returncode,1)
            self.assertEqual(r.stdout,b'')
            log.write_bytes(wire.replace(b'encoding=hex',b'encoding=raw',1))
            r=subprocess.run(command+['first'],capture_output=True,timeout=5)
            self.assertEqual(r.returncode,1)
            self.assertEqual(r.stdout,b'')

    def test_raw_lossy_and_repeat_compression_mutations_are_detected(self):
        raw="""LC_ALL=C od -An -v -tx1 "$D/konsole-attempt-$query_slot" | LC_ALL=C tr -d '[:space:]'"""
        variants=[H.FUNCTIONS.replace(raw,'cat "$D/konsole-attempt-$query_slot"'),
                  H.FUNCTIONS.replace('LC_ALL=C od -An -v -tx1 "$D/konsole-attempt-$query_slot"',
                                      """sed 's/FAIL//g' "$D/konsole-attempt-$query_slot" | LC_ALL=C od -An -v -tx1"""),
                  H.FUNCTIONS.replace('od -An -v -tx1','od -An -tx1')]
        for functions in variants:
            self.assertNotEqual(functions,H.FUNCTIONS)
            payload=b'FAIL\x00\r\n'+b'Z'*4096
            good=emit_slots({'first':payload})
            self.assertEqual(D.decode_history(good.stdout)['first'],payload)
            mutated=emit_slots({'first':payload},functions=functions)
            try: decoded=D.decode_history(mutated.stdout).get('first')
            except ValueError: decoded=None
            self.assertNotEqual(decoded,payload)
        raw_mutant=collision_run(functions=variants[0])[0]
        self.assertEqual(raw_mutant.returncode,0)
        self.assertFalse(strict_accept(raw_mutant.stdout.encode()))

    def test_only_emitter_changed_from_07fda66_probe(self):
        text=H.TEXT
        start=text.index('emit_terminal_attempts() {')
        end=text.index('# Diagnostic only: all samples',start)
        digest=hashlib.sha256((text[:start]+text[end:]).encode()).hexdigest()
        self.assertEqual(digest,'b31d3edc72934a1e6a613ac83a94fb2b828bfd102a7dfe4cd44d7f4d34de6b2e')

    def test_strict_parser_controls_remain_strict(self):
        self.assertTrue(strict_accept(b'normal\n'))
        for body,rc,panic in [(b'normal\n',1,False),(b'normal\n',0,True),
                              (b'FAIL\n',0,False),(b'/FAILURE\n',0,False),
                              (b' FAILED\n',0,False)]:
            self.assertFalse(strict_accept(body,rc,panic))

if __name__=='__main__': unittest.main()
