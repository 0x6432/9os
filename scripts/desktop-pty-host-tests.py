#!/usr/bin/env python3
"""PTY + focus readiness logic; mock commands are NOT GUI evidence."""
import os,pathlib,re,shlex,subprocess,tempfile,unittest
TEXT=pathlib.Path(os.environ.get('PLASMA_PROBE_SOURCE','userland/alpine/desktop-overlay/usr/bin/alpine-plasma-probe')).read_text()
BLOCK=TEXT.split('if [ "${ALPINE_PLASMA_INTERACTIVE:-0}" = 1 ];then\n',1)[1]
# Anchor to the actual final failure gate, not the first preceding if. Optional
# launch branches must remain in START so negative mutations reach real launches.
_BEFORE=BLOCK.split("\n  echo 'alpine-plasma: FAIL interactive",1)[0]
START,_COND=_BEFORE.rsplit('\n if ',1)
assert _COND.endswith(';then')
COND=_COND[:-len(';then')]
LEGACY_START,LEGACY_COND=re.match(r'(.*?)\n if (.*?);then\n  echo \'alpine-plasma: FAIL interactive',BLOCK,re.S).groups()
class Test(unittest.TestCase):
 def check(self,expected,delay='.1',result='/dev/pts/mock',status=0,bad_marker=False,focus_status=0,focus_delay='0',trace='0',legacy_extract=False):
  with tempfile.TemporaryDirectory() as d:
   root=pathlib.Path(d)
   (root/'konsole').write_text('#!/bin/sh\nwhile [ "$1" != -e ];do shift;done\nshift\nexec "$@"\n')
   (root/'tty').write_text('#!/bin/sh\nsleep '+delay+'\nprintf "%s\\n" '+shlex.quote(result)+'\nexit '+str(status)+'\n')
   (root/'dbus-send').write_text('#!/bin/sh\nexit 0\n')
   (root/'desktopa11y').write_text('#!/bin/sh\n[ "$1" = --terminal ] || exit 2\nsleep '+focus_delay+'\nexit '+str(focus_status)+'\n')
   for f in ['konsole','tty','dbus-send','desktopa11y']:(root/f).chmod(0o755)
   # Keep the mocked child alive after publication, like the interactive shell.
   start=(LEGACY_START if legacy_extract else START).replace('exec /bin/sh"','exec sleep 2"').replace('$(seq 1200)','$(seq 100)').replace('sleep .1','sleep .01')
   if bad_marker:start=start.replace('echo KDE_INTERACTIVE_PTY','echo INCOMPLETE_PTY')
   cmd='D='+shlex.quote(d)+'\n'+start+'\nif '+(LEGACY_COND if legacy_extract else COND)+';then r=1;else r=0;fi\nkill "$client" 2>/dev/null || true\nwait "$client" 2>/dev/null || true\nexit "$r"\n'
   run=subprocess.run(['/bin/sh','-c',cmd],env=dict(os.environ,PATH=d+':'+os.environ['PATH'],ALPINE_KONSOLE_INPUT_TRACE=trace),stdin=subprocess.DEVNULL,capture_output=True,text=True,timeout=5)
   self.assertEqual(run.returncode==0,expected,run.stdout+run.stderr)
 def test_delayed_tty_publication(self):
  for trace in ['0','1']:self.check(True,trace=trace)
 def test_immediate_tty_publication(self):
  for trace in ['0','1']:self.check(True,delay='0',trace=trace)
 def test_failed_tty(self):
  for trace in ['0','1']:self.check(False,status=1,trace=trace)
 def test_wrong_device(self):
  for trace in ['0','1']:self.check(False,result='/dev/null',trace=trace)
 def test_missing_shell_marker(self):
  for trace in ['0','1']:self.check(False,bad_marker=True,trace=trace)
 def test_unfocused_terminal_rejected(self):
  for trace in ['0','1']:self.check(False,focus_status=1,trace=trace)
 def test_delayed_focus_observation(self):
  for trace in ['0','1']:self.check(True,focus_delay='.2',trace=trace)
 def test_legacy_extractor_did_not_mutate_actual_launch(self):
  self.assertNotIn('echo KDE_INTERACTIVE_PTY',LEGACY_START)
  self.assertEqual(START.count('echo KDE_INTERACTIVE_PTY'),2)
  # Original broken fixture accepts the missing-marker mutation because its
  # START stopped at the new launch if, leaving both real commands unmutated.
  for trace in ['0','1']:self.check(True,bad_marker=True,trace=trace,legacy_extract=True)
if __name__=='__main__':unittest.main()
