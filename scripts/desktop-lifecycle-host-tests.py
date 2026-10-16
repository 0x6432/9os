#!/usr/bin/env python3
"""Exercise the actual physical-driver shell lifecycle, with explicit mock only."""
import os,pathlib,shlex,subprocess,tempfile,unittest
TEXT=pathlib.Path(__file__).with_name('alpine-kms-tests.sh').read_text()
TOKENS=shlex.split(TEXT,comments=True)
SPAWN=next(t for t in TOKENS if t.startswith('(if ALPINE_PLASMA_OUTPUT='))
FINISH=next(t for t in TOKENS if t.startswith('for i in $(seq 150)'))
MARKER='alpine-dolphin: PASSED real UID1000 directory listing, rename/create notifications and clean exit'
class Test(unittest.TestCase):
 def check(self,code,marker,expected,missing=False):
  with tempfile.TemporaryDirectory() as d:
   root=pathlib.Path(d);p=root/'alpine-plasma-user-probe'
   p.write_text('#!/bin/sh\n'+('printf "%s\\n" '+shlex.quote(MARKER)+'\n' if marker else 'echo incomplete\n')+'exit '+str(code)+'\n');p.chmod(0o755)
   def rewrite(s):return s.replace('/tmp/plasma-kms',str(root/'plasma-kms'))
   # Mock only the costly desktop program. Run the ACTUAL parent/witness/gate.
   cmd=rewrite(SPAWN)+'\nwait\n'
   if missing:cmd+='rm -f '+shlex.quote(str(root/'plasma-kms.status'))+'\n'
   cmd+=rewrite(FINISH).replace('$(seq 150)','$(seq 1)').replace('sleep 1','true')
   env=dict(os.environ,PATH=d+':'+os.environ['PATH'])
   result=subprocess.run(['/bin/sh','-c',cmd],env=env,capture_output=True,text=True,timeout=5)
   self.assertEqual(result.returncode==0,expected,result.stdout+result.stderr)
 def test_complete_success(self):self.check(0,True,True)
 def test_failure_even_with_success_marker(self):self.check(7,True,False)
 def test_success_without_clean_exit_marker(self):self.check(0,False,False)
 def test_missing_status_witness(self):self.check(0,True,False,missing=True)
 def test_signalled_session(self):self.check(143,True,False)
if __name__=='__main__':unittest.main()
