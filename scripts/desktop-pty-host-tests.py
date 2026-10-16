#!/usr/bin/env python3
"""Actual Konsole shell producer/readiness logic; mocked tty is NOT GUI evidence."""
import os,pathlib,re,shlex,subprocess,tempfile,unittest
TEXT=pathlib.Path(os.environ.get('PLASMA_PROBE_SOURCE','userland/alpine/desktop-overlay/usr/bin/alpine-plasma-probe')).read_text()
BLOCK=TEXT.split('if [ "${ALPINE_PLASMA_INTERACTIVE:-0}" = 1 ];then\n',1)[1]
START,COND=re.match(r'(.*?)\n if (.*?);then\n  echo \'alpine-plasma: FAIL interactive',BLOCK,re.S).groups()
class Test(unittest.TestCase):
 def check(self,expected,delay='.1',result='/dev/pts/mock',status=0,bad_marker=False):
  with tempfile.TemporaryDirectory() as d:
   root=pathlib.Path(d)
   (root/'konsole').write_text('#!/bin/sh\nwhile [ "$1" != -e ];do shift;done\nshift\nexec "$@"\n')
   (root/'tty').write_text('#!/bin/sh\nsleep '+delay+'\nprintf "%s\\n" '+shlex.quote(result)+'\nexit '+str(status)+'\n')
   for f in ['konsole','tty']:(root/f).chmod(0o755)
   # Keep the mocked child alive after publication, like the interactive shell.
   start=START.replace('exec /bin/sh"','exec sleep 2"').replace('$(seq 1200)','$(seq 100)').replace('sleep .1','sleep .01')
   if bad_marker:start=start.replace('echo KDE_INTERACTIVE_PTY','echo INCOMPLETE_PTY')
   cmd='D='+shlex.quote(d)+'\n'+start+'\nif '+COND+';then r=1;else r=0;fi\nkill "$client" 2>/dev/null || true\nwait "$client" 2>/dev/null || true\nexit "$r"\n'
   run=subprocess.run(['/bin/sh','-c',cmd],env=dict(os.environ,PATH=d+':'+os.environ['PATH']),stdin=subprocess.DEVNULL,capture_output=True,text=True,timeout=5)
   self.assertEqual(run.returncode==0,expected,run.stdout+run.stderr)
 def test_delayed_tty_publication(self):self.check(True)
 def test_immediate_tty_publication(self):self.check(True,delay='0')
 def test_failed_tty(self):self.check(False,status=1)
 def test_wrong_device(self):self.check(False,result='/dev/null')
 def test_missing_shell_marker(self):self.check(False,bad_marker=True)
if __name__=='__main__':unittest.main()
