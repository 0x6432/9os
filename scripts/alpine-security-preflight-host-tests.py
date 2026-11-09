#!/usr/bin/env python3
"""Synthetic negative controls; never a guest or complete fixture audit."""
import hashlib, importlib.util, os, pathlib, tempfile, unittest
from unittest.mock import patch
ROOT=pathlib.Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('stagecheck',ROOT/'scripts/alpine-security-preflight.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class Controls(unittest.TestCase):
 def setUp(self):
  self.t=tempfile.TemporaryDirectory();self.addCleanup(self.t.cleanup);self.root=pathlib.Path(self.t.name)
  for name,mode in (m.BASE_MODES|m.DESKTOP_MODES).items():
   p=self.root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(name.encode());p.chmod(mode)
  for name in m.CAPS|m.DESKTOP_CAPS:
   p=self.root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b'inert')
  (self.root/'bin/su').symlink_to('/bin/bbsuid');(self.root/'bin/sh').symlink_to('/bin/busybox')
  self.hashes={n:hashlib.sha256(n.encode()).hexdigest() for n in m.HASHES}
 def inspect(self,profile='desktop',caps=None):
  values=m.CAPS|m.DESKTOP_CAPS if caps is None else caps
  def xattr(p,*args,**kwargs):
   name=str(p.relative_to(self.root))
   if name not in values:raise OSError('missing xattr')
   return values[name]
  # Production CI invokes this checker via sudo because bbsuid is 04111.
  # The unprivileged synthetic test emulates a privileged read of this one
  # private temporary file, restoring its exact mode immediately afterwards.
  original_read=pathlib.Path.read_bytes
  def read(path):
   if path == self.root/'bin/bbsuid' and not path.is_symlink():
    mode=path.stat().st_mode & 0o7777;path.chmod(mode|0o400)
    try:return original_read(path)
    finally:path.chmod(mode)
   return original_read(path)
  with patch.object(m,'HASHES',self.hashes),patch.object(m.os,'getxattr',side_effect=xattr),patch.object(pathlib.Path,'read_bytes',read):return m.inspect(self.root,profile)
 def test_valid_and_base_scope(self):
  self.assertEqual(self.inspect(),[])
  (self.root/'usr/sbin/unix_chkpwd').unlink()
  self.assertTrue(self.inspect());self.assertEqual(self.inspect('base'),[])
 def test_modes_bytes_links_and_capabilities_fail(self):
  for field in ('mode','content','link','xattr','symlink'):
   with self.subTest(field=field):
    self.setUp();p=self.root/'bin/bbsuid'
    caps=m.CAPS|m.DESKTOP_CAPS
    if field=='mode':p.chmod(0o111)
    if field=='content':p.chmod(0o4311);p.write_bytes(b'changed');p.chmod(0o4111)
    if field=='link':(self.root/'bin/su').unlink();(self.root/'bin/su').symlink_to('/bin/busybox')
    if field=='xattr':caps={}
    if field=='symlink':p.unlink();p.symlink_to('busybox')
    self.assertTrue(self.inspect(caps=caps))
if __name__=='__main__':unittest.main()
