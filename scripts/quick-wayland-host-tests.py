#!/usr/bin/env python3
"""Wrapper controls only: no claim of Qt compilation or compositor rendering."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

TOP=Path(__file__).resolve().parents[1]
class Wrapper(unittest.TestCase):
    def invoke(self,user='',status=0):
        with tempfile.TemporaryDirectory() as tmp:
            driver=Path(tmp)/'qtwaylandsmoke'
            driver.write_text('''#!/bin/sh
set -eu
[ "$QTWAYLAND_TEST_CLIENT" = qtquickwaylandtest ]
[ "$QT_QUICK_BACKEND" = software ]
[ "$QV4_FORCE_INTERPRETER" = 1 ]
[ "$QML_DISABLE_DISK_CACHE" = 1 ]
[ -z "${QML_IMPORT_PATH+x}${QML2_IMPORT_PATH+x}${QT_PLUGIN_PATH+x}" ]
[ "${QTWAYLAND_CLIENT_USER:-}" = "$EXPECTED_USER" ]
exit "$EXPECTED_STATUS"
''')
            driver.chmod(0o755)
            env=dict(os.environ,PATH=tmp+':'+os.environ['PATH'],QTWAYLAND_TEST_CLIENT='qtgltest',QT_QUICK_BACKEND='opengl',QV4_FORCE_INTERPRETER='0',QML_IMPORT_PATH='/poison',QML2_IMPORT_PATH='/poison',QT_PLUGIN_PATH='/poison',EXPECTED_USER=user,EXPECTED_STATUS=str(status))
            if user:env['QTWAYLAND_CLIENT_USER']=user
            else:env.pop('QTWAYLAND_CLIENT_USER',None)
            return subprocess.run([str(TOP/'userland/ports/scripts/qtquickwaylandsmoke')],env=env,capture_output=True,text=True,timeout=5)
    def test_root_wrapper_explicit_software_no_foreign_paths(self):
        r=self.invoke();self.assertEqual(r.returncode,0,r.stderr)
    def test_uid1000_selection_preserved(self):
        r=self.invoke('user');self.assertEqual(r.returncode,0,r.stderr)
    def test_failure_status_not_swallowed(self):
        r=self.invoke(status=23);self.assertEqual(r.returncode,23,r.stderr)
    def test_uses_real_wayland_harness_and_required_frame_marker(self):
        s=(TOP/'userland/ports/scripts/qtwaylandsmoke').read_text()
        self.assertIn('qtquickwaylandtest) CLIENT=qtquickwaylandtest; export QT_QPA_PLATFORM=wayland ;;',s)
        self.assertIn('"$CLIENT: compositor frame 3"',s)
        self.assertIn('[ "$mapped" != 1 ]',s)
        self.assertIn('wait "$client"; r=$?',s)
        self.assertIn('wait "$compositor"; r=$?',s)
if __name__=='__main__':unittest.main()
