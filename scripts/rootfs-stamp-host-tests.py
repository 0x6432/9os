#!/usr/bin/env python3
import importlib.util,pathlib,tempfile,unittest
spec=importlib.util.spec_from_file_location('stamp',pathlib.Path(__file__).with_name('rootfs-stamp.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class StampTests(unittest.TestCase):
    def test_names_mutations_and_stability(self):
        with tempfile.TemporaryDirectory() as d:
            root=pathlib.Path(d)/'root';root.mkdir();stamp=pathlib.Path(d)/'out/stamp'
            a=root/'Air Balloon #1 $photo.jpg';a.write_bytes(b'a')
            self.assertTrue(m.update(root,stamp));t=stamp.stat().st_mtime_ns
            self.assertFalse(m.update(root,stamp));self.assertEqual(stamp.stat().st_mtime_ns,t)
            a.write_bytes(b'longer');self.assertTrue(m.update(root,stamp))
            a.chmod(0o700);self.assertTrue(m.update(root,stamp))
            link=root/'space link';link.symlink_to(a.name);self.assertTrue(m.update(root,stamp))
            link.unlink();link.symlink_to('missing');self.assertTrue(m.update(root,stamp))
            a.unlink();self.assertTrue(m.update(root,stamp));self.assertFalse(m.update(root,stamp))
    def test_missing_root_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(ValueError):m.update(pathlib.Path(d)/'missing',pathlib.Path(d)/'out')
if __name__=='__main__':unittest.main()
