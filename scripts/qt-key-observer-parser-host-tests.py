#!/usr/bin/env python3
from importlib.machinery import SourceFileLoader
from pathlib import Path
import unittest,subprocess,tempfile,json,sys
M=SourceFileLoader('qt_submit_check',str(Path(__file__).with_name('qt-key-observer-check.py'))).load_module()
def fixture(repeat=False):
 rows=[M.START,M.LOADED];edges=[e for f in M.C.expected_frames(M.C.DEFAULT_TEXT)for e in f]
 for i,(code,state)in enumerate(edges+([(32,0),(32,1)]if repeat else [])):
  for off,phase,result in [(0,'begin',-1),(1,'end',1)]:
   rows.append(f'QT_KEY_TRACE seq={i} phase={phase} pid=900 clock_ok=1 mono_ns={i*2+off+1} source_ms=12345 type={6 if state else 7} key=68 modifiers=0 scan={code+8} sym=100 native=0 autorepeat={int(i>=len(edges))} count=1 result={result}')
 rows.append(M.END);return '\n'.join(rows)+'\n'
class Test(unittest.TestCase):
 def test_exact_and_replay(self):
  s=fixture();self.assertEqual(M.check(s)['physical_key_edges'],262);self.assertEqual(M.check(s+s)['identical_replays'],2)
 def test_repeats_retained_not_silenced(self):
  r=M.check(fixture(True));self.assertEqual(r['autorepeat_events'],2);self.assertEqual(r['autorepeat_scancodes'],[40,40])
 def test_strict_cli_reports_and_rejects_autorepeat(self):
  with tempfile.TemporaryDirectory()as d:
   p=Path(d)/'trace.log'
   for repeat in [False,True]:
    p.write_text(fixture(repeat))
    for strict in [False,True]:
     cmd=[sys.executable,str(Path(__file__).with_name('qt-key-observer-check.py'))]+(['--require-no-autorepeat']if strict else[])+[str(p)]
     r=subprocess.run(cmd,capture_output=True,text=True,timeout=5)
     self.assertEqual(r.returncode,int(repeat and strict));self.assertEqual(json.loads(r.stdout)['autorepeat_events'],2 if repeat else 0)
     if repeat and strict:self.assertIn('Qt generated autorepeat',r.stderr)
 def test_negative_controls(self):
  s=fixture()
  variants=[s.replace(M.LOADED,'',1),s.replace(M.END,'',1),s.replace('phase=end','phase=begin',1),s.replace('clock_ok=1','clock_ok=0',1),s.replace('scan=28','scan=29',1),s.replace('pid=900','pid=901',1),s.replace('result=-1','result=0',1),s.replace('mono_ns=2 ','mono_ns=0 ',1),s.replace('count=1','count=65536',1),s.replace('native=0','native=4294967296',1),s.replace(M.END,'QT_KEY_TRACE_TRUNCATED limit=4096\n'+M.END),s+s.replace('source_ms=12345','source_ms=12346')]
  for i,v in enumerate(variants):
   with self.subTest(i=i),self.assertRaises(ValueError):M.check(v)
if __name__=='__main__':unittest.main()
