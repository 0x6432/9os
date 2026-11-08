#!/usr/bin/env python3
"""Exercise actual ext2 metadata code on synthetic files; no guest or QEMU."""
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('owner_repair',ROOT/'scripts/alpine-desktop-owner-repair.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class Image:
    def __init__(self, base, *, bad_group=False, bad_mode=False, symlink=False, hardlink=False, wrong_bytes=False):
        self.root=base/'root';self.work=base/'work';self.work.mkdir();self.root.mkdir()
        for d in ('etc','usr/libexec','usr/sbin'): (self.root/d).mkdir(parents=True,exist_ok=True)
        (self.root/'etc/group').write_text('root:x:0:\nmessagebus:x:81:\nshadow:x:'+('0' if bad_group else '42')+':\n')
        a=self.root/'usr/libexec/dbus-daemon-launch-helper';b=self.root/'usr/sbin/unix_chkpwd'
        a.write_bytes(b'INERT SIGNED DBUS PAYLOAD');a.chmod(0o4750)
        if symlink:b.symlink_to('../libexec/dbus-daemon-launch-helper')
        else:
            b.write_bytes(b'WRONG' if wrong_bytes else b'INERT SIGNED PAM PAYLOAD');b.chmod(0o755 if bad_mode else 0o2755)
        if hardlink:(self.root/'usr/libexec/extra-link').hardlink_to(a)
        self.image=base/'image.part';self.image.touch();subprocess.run(['truncate','-s','32M',str(self.image)],check=True)
        subprocess.run(['mke2fs','-q','-F','-t','ext2','-d',str(self.root),str(self.image)],check=True,capture_output=True)
        for p in ('/usr/libexec/dbus-daemon-launch-helper','/usr/sbin/unix_chkpwd'):
            m.debugfs(self.image,'set_inode_field '+p+' uid 0',True);m.debugfs(self.image,'set_inode_field '+p+' gid 0',True)
        self.contracts=[('/usr/libexec/dbus-daemon-launch-helper',81,0o4750,hashlib.sha256(b'INERT SIGNED DBUS PAYLOAD').hexdigest()),
                        ('/usr/sbin/unix_chkpwd',42,0o2755,hashlib.sha256(b'INERT SIGNED PAM PAYLOAD').hexdigest())]
    def gids(self):return tuple(m.inode(self.image,c[0])[4] for c in self.contracts)

class Repair(unittest.TestCase):
    def test_repairs_both_groups_and_retains_mode_and_content(self):
        with tempfile.TemporaryDirectory() as td:
            f=Image(Path(td));self.assertEqual(f.gids(),(0,0));result=m.repair(f.image,f.contracts,f.work)
            self.assertEqual(f.gids(),(81,42));self.assertEqual(len(result),2)
            self.assertEqual(m.inode(f.image,f.contracts[0][0])[2],0o4750)
            self.assertEqual(m.inode(f.image,f.contracts[1][0])[2],0o2755)
    def test_incorrect_prerequisites_fail_before_any_group_change(self):
        for kwargs in ({'bad_group':True},{'bad_mode':True},{'symlink':True},{'hardlink':True},{'wrong_bytes':True}):
            with self.subTest(kwargs=kwargs),tempfile.TemporaryDirectory() as td:
                f=Image(Path(td),**kwargs)
                with self.assertRaises(ValueError):m.repair(f.image,f.contracts,f.work)
                self.assertEqual(m.inode(f.image,f.contracts[0][0])[4],0)
    def test_rejects_shared_and_non_temporary_images(self):
        with tempfile.TemporaryDirectory() as td:
            base=Path(td);f=Image(base);(base/'alias').hardlink_to(f.image)
            with self.assertRaisesRegex(ValueError,'unshared'):m.repair(f.image,f.contracts,f.work)
            (base/'alias').unlink();f.image.rename(base/'golden.raw')
            with self.assertRaisesRegex(ValueError,'temporary'):m.repair(base/'golden.raw',f.contracts,f.work)
    def test_mkdisk_refusal_does_not_publish_partition(self):
        with tempfile.TemporaryDirectory() as td:
            base=Path(td);f=Image(base);out=base/'disk.raw';cache=base/'empty-cache';cache.mkdir()
            env=dict(os.environ,ALPINE_DESKTOP_OWNER_POLICY='x86_64',ALPINE_DESKTOP_APK_CACHE=str(cache))
            p=subprocess.run([str(ROOT/'scripts/mkdisk.sh'),str(out),'64',str(f.root)],env=env,capture_output=True,text=True,timeout=20)
            self.assertNotEqual(p.returncode,0,p.stdout+p.stderr)
            self.assertIn('ownership repair refused',p.stderr)
            self.assertTrue((base/'disk.raw.part').exists())
            with out.open('rb') as disk:
                disk.seek(1048576+1024+56);self.assertNotEqual(disk.read(2),b'\x53\xef')
    def test_bad_policy_does_not_replace_existing_output(self):
        with tempfile.TemporaryDirectory() as td:
            base=Path(td);out=base/'disk.raw';out.write_bytes(b'existing data')
            env=dict(os.environ,ALPINE_DESKTOP_OWNER_POLICY='unknown')
            p=subprocess.run([str(ROOT/'scripts/mkdisk.sh'),str(out),'64',str(base)],env=env,capture_output=True,text=True,timeout=10)
            self.assertNotEqual(p.returncode,0);self.assertEqual(out.read_bytes(),b'existing data')

    def test_rejects_mismatched_lock_and_package_hashes(self):
        with tempfile.TemporaryDirectory() as td:
            old=m.LOCK;fake=Path(td)/'desktop.json';fake.write_text('{}');m.LOCK=fake
            try:
                with self.assertRaisesRegex(ValueError,'lock differs'):m.verified_contract(Path(td),Path(td))
            finally:m.LOCK=old
            with self.assertRaisesRegex(ValueError,'missing or mismatched policy APK'):m.verified_contract(Path(td),Path(td))

if __name__=='__main__':unittest.main()
