#!/usr/bin/env python3
"""Host-only controls; never runs an ISO, VM, fixture builder or workflow."""
import importlib.util
import os
from pathlib import Path
import stat
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('prototype', ROOT/'scripts/disk-root-prototype.py')
P = importlib.util.module_from_spec(spec); spec.loader.exec_module(P)
spec = importlib.util.spec_from_file_location('archive_check', ROOT/'scripts/initramfs-archive-check.py')
ARCHIVE = importlib.util.module_from_spec(spec); spec.loader.exec_module(ARCHIVE)

class Prototype(unittest.TestCase):
    def test_exact_archive_and_strict_configuration(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); output = root/'output'; P.create(output); P.verify(output)
            empty = root/'empty'; empty.mkdir(mode=0o755); empty.chmod(0o755)
            ARCHIVE.check(output/'initramfs.cpio', empty)
            config = (output/'limine.conf').read_text()
            self.assertEqual(config.count('cmdline:'), 1)
            self.assertIn('cmdline: root=/dev/vda1 root_required=1\n', config)
            self.assertNotIn('init=', config)
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o700)

    def test_existing_directory_and_symlink_never_replaced(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); old = root/'old'; old.mkdir(); (old/'evidence').write_text('keep')
            with self.assertRaises(FileExistsError): P.create(old)
            self.assertEqual((old/'evidence').read_text(), 'keep')
            link = root/'link'; link.symlink_to(old, target_is_directory=True)
            with self.assertRaises(FileExistsError): P.create(link)
            with self.assertRaises(ValueError): P.verify(link)

    def test_corrupt_missing_extra_and_symlink_files_are_refused(self):
        for scenario in ('archive', 'config', 'manifest', 'ready', 'missing', 'extra', 'symlink'):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as d:
                root=Path(d); out=root/'out'; P.create(out)
                targets={'archive':'initramfs.cpio', 'config':'limine.conf',
                         'manifest':'manifest.json', 'ready':'READY'}
                if scenario in targets: (out/targets[scenario]).write_bytes(b'altered')
                elif scenario=='missing': (out/'READY').unlink()
                elif scenario=='extra': (out/'extra').write_text('not allowed')
                else:
                    (out/'limine.conf').unlink(); (out/'limine.conf').symlink_to(root/'missing')
                with self.assertRaises(ValueError): P.verify(out)

    def test_interrupted_creation_is_incomplete_not_ready(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)/'out'; actual=P.write_new
            def fail_second(path, data):
                if path.name == 'limine.conf': raise OSError('injected write failure')
                return actual(path, data)
            with mock.patch.object(P, 'write_new', side_effect=fail_second):
                with self.assertRaises(OSError): P.create(out)
            self.assertTrue((out/'initramfs.cpio').exists())
            self.assertFalse((out/'READY').exists())
            with self.assertRaises(ValueError): P.verify(out)
            with self.assertRaises(FileExistsError): P.create(out)

    def test_bad_mode_and_hardlink_are_refused(self):
        for scenario in ('directory_mode', 'file_mode', 'hardlink'):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as d:
                root=Path(d); out=root/'out'; P.create(out)
                if scenario == 'directory_mode': out.chmod(0o777)
                elif scenario == 'file_mode': (out/'limine.conf').chmod(0o666)
                else: os.link(out/'limine.conf', root/'hardlink')
                with self.assertRaises(ValueError): P.verify(out)

    def test_corruption_before_ready_and_failed_post_ready_verify(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)/'bad'; actual=P.write_new
            def corrupt(path, data):
                return actual(path, b'bad' if path.name == 'limine.conf' else data)
            with mock.patch.object(P, 'write_new', side_effect=corrupt):
                with self.assertRaises(ValueError): P.create(out)
            self.assertFalse((out/'READY').exists())
            with self.assertRaises(ValueError): P.verify(out)
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)/'bad'
            with mock.patch.object(P, 'verify', side_effect=ValueError('injected post-ready failure')):
                with self.assertRaises(ValueError): P.create(out)
            self.assertFalse((out/'READY').exists())
            with self.assertRaises(ValueError): P.verify(out)

    def test_independent_outputs_are_identical(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); P.create(root/'one'); P.create(root/'two')
            for name in P.FILES:
                self.assertEqual((root/'one'/name).read_bytes(), (root/'two'/name).read_bytes())

if __name__ == '__main__': unittest.main()
