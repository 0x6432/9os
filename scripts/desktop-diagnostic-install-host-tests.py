#!/usr/bin/env python3
"""Exercise the real desktop installer with inert files; never boots or builds."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'scripts/alpine-desktop-install-tests.sh').read_text()
WRAPPER = (ROOT / 'scripts/alpine-desktop-tests.sh').read_text()
CONSUMERS = ('fallocatetest ptracestoptest unixcredtest procinfotest dircookietest '
             'proctasktest mountinfotest pimutextest ioctlwidthtest desktopframecheck '
             'desktopa11y desktopa11y-dbus trace-snapshot trace-snapshot-target '
             'inputeventcapture').split()


def run_installer(source=SOURCE, missing=None, wrapper_mode=True):
    with tempfile.TemporaryDirectory(prefix='desktop-install-inert-') as tmp:
        root = Path(tmp)
        scripts = root / 'scripts'; scripts.mkdir()
        wrapper = scripts / 'alpine-desktop-tests.sh'; wrapper.write_text(WRAPPER)
        installer = scripts / 'alpine-desktop-install-tests.sh'; installer.write_text(source); installer.chmod(0o755)
        original = root / 'userland/alpine-root-x86_64'
        target = root / 'userland/alpine-desktop-root-x86_64'
        for directory in (original / 'usr/bin', original / 'usr/lib/9os-tests',
                          target / 'usr/bin', target / 'usr/lib', root / 'fakebin'):
            directory.mkdir(parents=True)
        files = [original / 'usr/bin' / name for name in CONSUMERS]
        files += [original / 'usr/bin/qt-key-observer-caller',
                  original / 'usr/lib/lib9os-qt-key-observer.so',
                  original / 'usr/lib/9os-tests/libqt-key-observer-mock.so']
        for f in files:
            f.write_text('INERT ' + f.name + '\n')
            f.chmod(0o755)
        if missing:
            (original / 'usr/bin' / missing).unlink()
        disk = scripts / 'mkdisk.sh'
        disk.write_text('#!/bin/sh\nprintf disk > "$MARK_DISK"\n')
        disk.chmod(0o755)
        fake_python = root / 'fakebin/python3'
        fake_python.write_text('#!/bin/sh\nprintf fake-runner > "$MARK_RUNNER"\nexit 0\n')
        fake_python.chmod(0o755)
        env = dict(os.environ, PATH=str(root / 'fakebin') + ':/usr/bin:/bin',
                   MARK_DISK=str(root / 'disk-called'), MARK_RUNNER=str(root / 'runner-called'))
        r = subprocess.run(['/bin/sh', str(wrapper if wrapper_mode else installer), 'x86_64'], cwd=root, env=env,
                           capture_output=True, text=True, timeout=10)
        capture = target / 'usr/bin/inputeventcapture'
        return (r.returncode, capture.read_text() if capture.exists() else None,
                (capture.stat().st_mode & 0o111) if capture.exists() else 0,
                (root / 'disk-called').exists(), (root / 'runner-called').exists())


class DesktopDiagnosticInstall(unittest.TestCase):
    def test_install_only_never_packs_or_runs(self):
        rc, content, executable, disk, runner = run_installer(wrapper_mode=False)
        self.assertEqual((rc, content, bool(executable), disk, runner),
                         (0, 'INERT inputeventcapture\n', True, False, False))

    def test_actual_installer_copies_raw_collector_before_disk_or_runner(self):
        rc, content, executable, disk, runner = run_installer()
        self.assertEqual((rc, content, bool(executable), disk, runner),
                         (0, 'INERT inputeventcapture\n', True, True, True))

    def test_missing_collector_stops_before_disk_and_runner(self):
        rc, content, executable, disk, runner = run_installer(missing='inputeventcapture')
        self.assertNotEqual(rc, 0)
        self.assertEqual((content, executable, disk, runner), (None, 0, False, False))

    def test_removal_mutation_is_detected(self):
        mutant = SOURCE.replace(' trace-snapshot-target inputeventcapture;do cp',
                                ' trace-snapshot-target;do cp')
        self.assertNotEqual(mutant, SOURCE)
        rc, content, executable, disk, runner = run_installer(source=mutant)
        self.assertEqual(rc, 0)  # old installer silently proceeds
        self.assertIsNone(content)  # positive collector check rejects it
        self.assertEqual(executable, 0)
        self.assertTrue(disk and runner)


if __name__ == '__main__':
    unittest.main()
