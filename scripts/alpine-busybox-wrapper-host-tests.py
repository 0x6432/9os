#!/usr/bin/env python3
"""Synthetic stage controls only; not an Alpine package install or guest test."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('builder', ROOT / 'userland/alpine/build.py')
builder = importlib.util.module_from_spec(spec); spec.loader.exec_module(builder)

class WrapperPolicy(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root/'bin').mkdir(); (self.root/'usr/bin').mkdir(parents=True)
        for name,mode in [('bin/busybox',0o755),('bin/bbsuid',0o4111)]:
            p=self.root/name; p.write_bytes(b'inert');p.chmod(mode)
        for name in builder.BBSUID_LINKS:
            (self.root/name).symlink_to('/bin/busybox')

    def test_redirects_only_allowed_existing_busybox_links(self):
        (self.root/'usr/bin/passwd').unlink();(self.root/'usr/bin/passwd').write_text('separate shadow binary')
        (self.root/'usr/bin/vlock').unlink();(self.root/'usr/bin/vlock').symlink_to('/other/vlock')
        (self.root/'bin/sh').symlink_to('/bin/busybox')
        builder.configure_busybox_privileges(self.root)
        for name in ('bin/su','bin/mount','bin/umount','usr/bin/traceroute'):
            self.assertEqual(os.readlink(self.root/name),'/bin/bbsuid')
        self.assertEqual(os.readlink(self.root/'bin/sh'),'/bin/busybox')
        self.assertEqual(os.readlink(self.root/'usr/bin/vlock'),'/other/vlock')
        self.assertEqual((self.root/'usr/bin/passwd').read_text(),'separate shadow binary')
        self.assertEqual((self.root/'bin/busybox').stat().st_mode & 0o7777,0o755)
        self.assertEqual((self.root/'bin/bbsuid').stat().st_mode & 0o7777,0o4111)

    def test_invalid_inputs_fail_before_symlink_mutation(self):
        for bad in ('missing-wrapper','wrapper-mode','busybox-mode','symlink-wrapper','missing-su','wrong-su'):
            with self.subTest(bad=bad):
                self.setUp();root=self.root
                if bad=='missing-wrapper': (root/'bin/bbsuid').unlink()
                if bad=='wrapper-mode': (root/'bin/bbsuid').chmod(0o755)
                if bad=='busybox-mode': (root/'bin/busybox').chmod(0o4755)
                if bad=='symlink-wrapper': (root/'bin/bbsuid').unlink();(root/'bin/bbsuid').symlink_to('busybox')
                if bad=='missing-su': (root/'bin/su').unlink()
                if bad=='wrong-su': (root/'bin/su').unlink();(root/'bin/su').symlink_to('/other/su')
                with self.assertRaises(ValueError):builder.configure_busybox_privileges(root)
                self.assertEqual(os.readlink(root/'bin/mount'),'/bin/busybox')

if __name__=='__main__':unittest.main()
