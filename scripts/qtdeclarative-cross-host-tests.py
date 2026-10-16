#!/usr/bin/env python3
"""Execute the actual pinned tools CMake list with native/cross feature models.

Only configuration control flow is tested; not a source Qt library build.
"""
from importlib.machinery import SourceFileLoader
from pathlib import Path
import subprocess
import tempfile
import unittest

TOP = Path(__file__).resolve().parents[1]
PATCH = SourceFileLoader('cross_tools', str(TOP / 'userland/ports/qtdeclarative-cross-tools.py')).load_module()
FIXTURE = (TOP / 'scripts/fixtures/qtdeclarative-tools-6.8.3.cmake').read_text()


class CrossTools(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.file = self.root / 'tools.cmake'
        self.file.write_text(FIXTURE)

    def run_cmake(self, cross, quick, patch=True):
        if patch:
            PATCH.apply(self.file)
        # Stub only subdirectory entry, preserving the upstream if/target logic.
        # Imported interface targets model the GUI/Quick dependency visibility.
        src = ('cmake_minimum_required(VERSION 3.20)\nproject(Control NONE)\n'
               f'set(CMAKE_CROSSCOMPILING {"TRUE" if cross else "FALSE"})\n'
               'set(QT_FEATURE_commandlineparser ON)\nset(QT_FEATURE_thread ON)\n'
               'set(QT_FEATURE_process ON)\nset(QT_FEATURE_regularexpression ON)\n'
               'set(QT_BUILD_SHARED_LIBS ON)\n'
               + ('add_library(Qt::Quick INTERFACE IMPORTED)\nadd_library(Qt::QuickTest INTERFACE IMPORTED)\n' if quick else '') +
               'function(add_subdirectory name)\n'
               'file(APPEND "${CMAKE_BINARY_DIR}/entered.txt" "${name}\\n")\n'
               'if(CMAKE_CROSSCOMPILING AND (name STREQUAL "qmltime" OR name STREQUAL "qmlplugindump" OR name STREQUAL "qmltestrunner"))\n'
               'message(FATAL_ERROR "Missing headless host GUI tool: ${name}")\n'
               'endif()\nendfunction()\ninclude("${CMAKE_SOURCE_DIR}/tools.cmake")\n')
        (self.root / 'CMakeLists.txt').write_text(src)
        build = self.root / f'build-{cross}-{quick}-{patch}'
        r = subprocess.run(['cmake', '-S', str(self.root), '-B', str(build)],
                           capture_output=True, text=True, timeout=15)
        entered = (build / 'entered.txt').read_text().splitlines()
        return r, entered

    def test_cross_quick_omits_only_optional_host_gui_tools(self):
        r, entered = self.run_cmake(True, True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        for name in ('qmltime', 'qmlplugindump', 'qmltestrunner'):
            self.assertNotIn(name, entered)
        for name in ('qmlscene', 'qml', 'qmltyperegistrar', 'qmlimportscanner', 'qmllint', 'qmltc'):
            self.assertIn(name, entered)

    def test_unpatched_cross_quick_reproduces_failure(self):
        r, _ = self.run_cmake(True, True, False)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('Missing headless host GUI tool: qmltime', r.stderr)

    def test_native_quick_retains_all_tools(self):
        r, entered = self.run_cmake(False, True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('qmltime', entered)
        self.assertIn('qmlplugindump', entered)
        self.assertIn('qmltestrunner', entered)

    def test_headless_native_generators_unchanged(self):
        r, entered = self.run_cmake(False, False)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn('qmltime', entered)
        self.assertIn('qmltyperegistrar', entered)
        self.assertIn('qmlimportscanner', entered)

    def test_legacy_patch_reproduces_missing_qmltestrunner(self):
        self.file.write_text(PATCH.patched(FIXTURE, PATCH.LEGACY_TOOLS))
        r, _ = self.run_cmake(True, True, False)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('Missing headless host GUI tool: qmltestrunner', r.stderr)

    def test_legacy_patch_upgrades_and_is_idempotent(self):
        self.file.write_text(PATCH.patched(FIXTURE, PATCH.LEGACY_TOOLS))
        PATCH.apply(self.file)
        expected = PATCH.patched(FIXTURE)
        self.assertEqual(self.file.read_text(), expected)
        PATCH.apply(self.file)
        self.assertEqual(self.file.read_text(), expected)

    def test_idempotence_and_changed_source_rejection(self):
        PATCH.apply(self.file)
        first = self.file.read_bytes()
        PATCH.apply(self.file)
        self.assertEqual(first, self.file.read_bytes())
        self.file.write_text(self.file.read_text() + '# unexpected change\n')
        with self.assertRaisesRegex(ValueError, 'Unexpected Qt'):
            PATCH.apply(self.file)

    def test_partial_patch_rejected(self):
        PATCH.apply(self.file)
        self.file.write_text(self.file.read_text().replace('                add_subdirectory(qmltime)',
                                                        '                add_subdirectory(other)', 1))
        with self.assertRaises(ValueError):
            PATCH.apply(self.file)


if __name__ == '__main__':
    unittest.main()
