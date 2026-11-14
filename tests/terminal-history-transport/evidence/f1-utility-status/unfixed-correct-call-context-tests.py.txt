#!/usr/bin/env python3
"""Preserve F1 before repair: correct bytes + nonzero utility exit is unobserved."""
from pathlib import Path
import hashlib,importlib.util,unittest,os,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
SPEC=importlib.util.spec_from_file_location('transport',ROOT/'scripts/terminal-history-transport-host-tests.py')
T=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(T)

class OriginalF1(unittest.TestCase):
 def test_complete_correct_od_bytes_then_nonzero_remain_unobserved(self):
  self.observe('od',7)
 def test_complete_correct_tr_bytes_then_nonzero_remain_unobserved(self):
  self.observe('tr',9)
 def observe(self,utility,status):
  payload=b'FAIL\x00\r\n'+bytes(range(256))
  override=f'\n{utility}() {{ command {utility} "$@"; return {status}; }}\n'
  with tempfile.TemporaryDirectory(prefix='history-f1-original-') as tmp:
   (Path(tmp)/'konsole-attempt-first').write_bytes(payload)
   # Actual production invokes the emitter in an OR-list; direct invocation
   # under sh -e is a different context and does not reproduce masked tr status.
   script=T.H.FUNCTIONS+override+'\nquery_attempt=2;query_recording_failed=0\nemit_terminal_attempts || true\n'
   r=subprocess.run(['/bin/sh','-eu','-c',script],env={**os.environ,'D':tmp},capture_output=True,timeout=5)
  self.assertEqual(r.returncode,0,r.stderr)
  self.assertEqual(T.D.decode_history(r.stdout),{'first':payload})
  self.assertNotIn(b'TRANSPORT_UNKNOWN',r.stdout)
  print(f'PRESERVED F1: {utility} emitted exact bytes then exit{status}; emitter rc0, decoder accepted, no transport status')

if __name__=='__main__':
 print('Unfixed source SHA256:',hashlib.sha256(T.H.TEXT.encode()).hexdigest(),flush=True)
 unittest.main(verbosity=2)
