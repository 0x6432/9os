#!/usr/bin/env python3
"""Offline host-only controls. Usage: python3 SCRIPT CACHE (no guest boot)."""
import importlib.util, os, pathlib, shutil, stat, subprocess, sys, tarfile, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('repair',ROOT/'scripts/alpine-userns-mode-repair.py');repair=importlib.util.module_from_spec(spec);spec.loader.exec_module(repair)
CACHE=None
class Controls(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory(prefix='m40-mode-control-');self.addCleanup(self.tmp.cleanup)
  self.stage=pathlib.Path(self.tmp.name)/'stage';self.stage.mkdir()
  for apk,_,name,_,mode,_,_ in repair.FILES:
   with tarfile.open(CACHE/apk,'r:gz',ignore_zeros=True) as tar:
    members=[x for x in tar if x.name==name];self.assertEqual(len(members),1)
    raw=tar.extractfile(members[0]).read()
   path=self.stage/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(raw);path.chmod(mode & ~0o6000)
 def run_repair(self,cache=None):
  return subprocess.run([sys.executable,str(ROOT/'scripts/alpine-userns-mode-repair.py'),str(self.stage),'--profile','desktop','--cache',str(cache or CACHE)],capture_output=True,text=True)
 def unchanged_first(self):
  self.assertEqual(stat.S_IMODE((self.stage/'bin/bbsuid').stat().st_mode),0o111)
 def test_restore_and_idempotent(self):
  for _ in range(2):
   p=self.run_repair();self.assertEqual(p.returncode,0,p.stderr)
   for _,_,name,_,mode,_,_ in repair.FILES:self.assertEqual(stat.S_IMODE((self.stage/name).stat().st_mode),mode)
 def test_tampered_payload_rejected_before_any_change(self):
  (self.stage/'usr/bin/passwd').write_bytes(b'tampered');p=self.run_repair();self.assertNotEqual(p.returncode,0);self.unchanged_first()
 def test_wrong_mode_rejected_before_any_change(self):
  (self.stage/'usr/bin/passwd').chmod(0o777);p=self.run_repair();self.assertNotEqual(p.returncode,0);self.unchanged_first()
 def test_symlink_rejected_before_any_change(self):
  p=self.stage/'usr/bin/passwd';p.unlink();p.symlink_to('/etc/passwd');out=self.run_repair();self.assertNotEqual(out.returncode,0);self.unchanged_first()
 def test_hardlink_rejected_before_any_change(self):
  os.link(self.stage/'usr/bin/passwd',self.stage/'usr/bin/other');out=self.run_repair();self.assertNotEqual(out.returncode,0);self.unchanged_first()
 def test_corrupt_signed_apk_rejected_before_any_change(self):
  c=pathlib.Path(self.tmp.name)/'cache';c.mkdir()
  for p in CACHE.iterdir():
   if p.name in {f[0] for f in repair.FILES}|{'alpine-minirootfs-3.22.6-x86_64.tar.gz'}:shutil.copyfile(p,c/p.name)
  with (c/repair.FILES[0][0]).open('ab') as f:f.write(b'tamper')
  out=self.run_repair(c);self.assertNotEqual(out.returncode,0);self.unchanged_first()

def main():
 global CACHE
 if len(sys.argv)==3 and sys.argv[1]=='--inner':
  CACHE=pathlib.Path(sys.argv[2]).resolve();sys.argv=[sys.argv[0]];unittest.main();return
 if len(sys.argv)!=2:raise SystemExit(__doc__)
 if os.geteuid()==0:raise SystemExit('refusing to run as host root')
 p=subprocess.run(['unshare','-Urn',sys.executable,str(pathlib.Path(__file__).resolve()),'--inner',str(pathlib.Path(sys.argv[1]).resolve())]);sys.exit(p.returncode)
if __name__=='__main__':main()
