#!/usr/bin/env python3
"""Actual backend shell dispatch; synthetic commands, never GUI proof."""
from pathlib import Path
import os,subprocess,tempfile,unittest
TOP=Path(__file__).resolve().parents[1]
PROBE=TOP/'userland/alpine/desktop-overlay/usr/bin/alpine-plasma-probe'
TEXT=PROBE.read_text()
FUNCTION=TEXT[TEXT.index('focused_terminal_query() {'):TEXT.index('terminal_dbus_diagnostic() {')]
class Backend(unittest.TestCase):
 def execute(self,mode=None,rc=0):
  with tempfile.TemporaryDirectory()as t:
   env={**os.environ,'D':t,'MOCK_RC':str(rc)};env.pop('ALPINE_TERMINAL_QUERY',None)
   if mode is not None:env['ALPINE_TERMINAL_QUERY']=mode
   s='timeout() { printf "%s\\n" "$*" >> "$D/calls";return "$MOCK_RC"; }\n'+FUNCTION+'\nfocused_terminal_query 777\n'
   r=subprocess.run(['/bin/sh','-eu','-c',s],env=env,text=True,capture_output=True,timeout=5)
   p=Path(t)/'calls';return r,p.read_text().splitlines()if p.exists()else[]
 def test_default_and_explicit_original_backend_unchanged(self):
  for mode in [None,'','atspi']:
   r,calls=self.execute(mode);self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(calls,['-k 2 -s TERM 8 desktopa11y --terminal 777'])
 def test_live_backend_has_same_bound_real_pid_no_fallback(self):
  for rc in [0,1,2,124,143]:
   r,calls=self.execute('live-dbus',rc);self.assertEqual(r.returncode,rc,r.stderr);self.assertEqual(calls,['-k 2 -s TERM 8 desktopa11y-dbus 777'])
 def test_unknown_dispatch_never_runs_either_helper(self):
  for mode in ['unknown','live-dbus;false','AT-SPI']:
   r,calls=self.execute(mode);self.assertEqual(r.returncode,2);self.assertEqual(calls,[])
 def test_invalid_configuration_rejected_before_session_start(self):
  for script in [PROBE,TOP/'scripts/alpine-kms-tests.sh']:
   r=subprocess.run(['/bin/sh',str(script)],env={**os.environ,'ALPINE_TERMINAL_QUERY':'bad'},capture_output=True,text=True,timeout=5)
   self.assertEqual(r.returncode,2,r.stderr);self.assertIn('ALPINE_TERMINAL_QUERY must be',r.stderr)
 def test_original_deadline_failure_exit_and_input_witness_retained(self):
  self.assertIn('deadline=$(( $(date +%s) + 120 ))',TEXT)
  gate=TEXT[TEXT.index(' pty_ready=0;'):TEXT.index(' echo KONSOLE_FOCUSED_TERMINAL_READY')]
  self.assertEqual(gate.count('if focused_terminal_query "$client"'),1)
  self.assertIn('terminal_dbus_diagnostic FAILURE_ONLY || true',gate)
  self.assertIn('  exit 1\n',gate)
  self.assertNotIn('ALPINE_PLASMA_INPUT_READY',gate)
if __name__=='__main__':unittest.main()
