#!/usr/bin/env python3
"""Synthetic result-rejection controls. These are not boot/GUI evidence."""
from pathlib import Path
import importlib.util,unittest
P=Path(__file__).with_name('m40-cold-run.py');spec=importlib.util.spec_from_file_location('plan',P);plan=importlib.util.module_from_spec(spec);spec.loader.exec_module(plan)
W='trace-snapshot-selftest: PASSED 3/3 synthetic process controls; not GUI proof\n'*2+'KONSOLE_FOCUSED_TERMINAL_READY\nKONSOLE_READINESS_BACKEND=live-dbus\n'
class Controls(unittest.TestCase):
 def test_exact_positive_shapes_only(self):
  self.assertTrue(plan.evaluate('guest1',W+'18/18 passed\n',0,0,True))
  self.assertTrue(plan.evaluate('linux1',W+'FAIL   grep -q "^violations 0" /proc/lockdep\n17/18 passed\n',1,0,True))
 def test_prior_failure_unknown_or_missing_evidence_never_accepted(self):
  good=W+'18/18 passed\n'
  for text,rc,raw,integrity in [(good,1,0,True),(good,0,1,True),(good,0,0,False),(W+'8/18 passed\n',1,0,True),(W,0,0,True),(good+'18/18 passed\n',0,0,True),(good+'FAIL   anything\n',0,0,True),(good.replace('KONSOLE_FOCUSED_TERMINAL_READY',''),0,0,True),(good.replace('live-dbus','atspi'),0,0,True),(good.replace('KONSOLE_FOCUSED_TERMINAL_READY','echo KONSOLE_FOCUSED_TERMINAL_READY'),0,0,True),(good.replace('KONSOLE_READINESS_BACKEND=live-dbus','echo KONSOLE_READINESS_BACKEND=live-dbus'),0,0,True),(good+'QT_KEY_TRACE_LOADED\n',0,0,True),(good+'WAYLAND_KEY_TRACE anything\n',0,0,True),(good.replace(W.splitlines()[0]+'\n','',1),0,0,True)]:
   with self.subTest(text=text,rc=rc,raw=raw,integrity=integrity):self.assertFalse(plan.evaluate('guest1',text,rc,raw,integrity))
 def test_linux_cannot_waive_other_failures_or_raw_capture(self):
  s=W+'FAIL   grep -q "^violations 0" /proc/lockdep\n17/18 passed\n'
  for changed in [s.replace('lockdep','other'),s.replace('17/18','16/18'),s.replace('FAIL   ','PASS   '),s+'FAIL   other\n']:
   self.assertFalse(plan.evaluate('linux1',changed,1,0,True))
  self.assertFalse(plan.evaluate('linux1',s,1,1,True))
 def test_exact_two_host_cpus_required_without_fallback(self):
  self.assertEqual(plan.validate_cpu_selection([2,4],[1,2,3,4]),[2,4])
  for selected,available in [([2],[2]),([2,2],[2]),([4,2],[2,4]),([2,4,6],[2,4,6]),([-1,2],[-1,2]),([2,4],[2]),([True,2],[1,2])]:
   with self.subTest(selected=selected,available=available):
    with self.assertRaises(AssertionError):plan.validate_cpu_selection(selected,available)
  self.assertIn('os.sched_setaffinity(0,selected)',P.read_text())
  self.assertIn("D/'host-profile.json'",P.with_name('m40-cold-prepare.py').read_text())
 def test_no_active_fixture_mutation_and_unconditional_uploads(self):
  driver=P.with_name('m40-cold-driver.sh').read_text()
  self.assertNotIn('mkdisk.sh',driver);self.assertIn('-snapshot',driver)
  self.assertIn('--ready-step 3',driver);self.assertIn('ALPINE_TERMINAL_QUERY=live-dbus',driver)
  self.assertIn('input-capture-session',driver)
  source=P.read_text();self.assertIn("assert not result.exists(),'no retries'",source)
  workflow=P.parents[1]/'.github/workflows/alpine.yml';text=workflow.read_text()
  self.assertEqual(text.count('if: always()'),5)
  self.assertEqual(text.count("if: ${{ !cancelled() && steps.linux.outcome == 'success' }}"),3)
  for sample in ['guest1','guest2','guest3']:
   self.assertIn('run: python3 scripts/m40-cold-run.py '+sample,text)
   self.assertTrue(plan.evaluate(sample,W+'18/18 passed\n',0,0,True))
   self.assertFalse(plan.evaluate(sample,W+'8/18 passed\n',1,0,True))
  prep=P.with_name('m40-cold-prepare.py').read_text()
  self.assertEqual(prep.count("['linux1','guest1','guest2','guest3']"),3)
  self.assertIn("files += [D/'adapters'/sample/'make' for sample in ['linux1','guest1','guest2','guest3']]",prep)
  self.assertIn('07fda66bd9a7904e77b7f3e1051df243ee1e4d21',prep)
  self.assertIn('07fda66bd9a7904e77b7f3e1051df243ee1e4d21',text)
  self.assertNotIn('pull_request:',text);self.assertNotIn('continue-on-error:',text)
if __name__=='__main__':unittest.main()
