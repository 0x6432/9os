#!/usr/bin/env python3
"""Actual-shell opt-in/PID controls; no real Wayland/Qt delivery claim."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'userland/alpine/desktop-overlay/usr/bin/alpine-plasma-probe'


class Launch(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)
        self.source = SOURCE.read_text()
        start = self.source.index(' if [ "${ALPINE_KONSOLE_INPUT_TRACE:-0}" = 1 ];then\n', self.source.index('export QT_LINUX_ACCESSIBILITY_ALWAYS_ON'))
        end = self.source.index(' fi\n', start)+len(' fi\n')
        self.block = self.source[start:end]
        mock = self.root/'konsole'
        mock.write_text('#!/bin/sh\nprintf "pid=%s debug=%s\\n" "$$" "${WAYLAND_DEBUG-unset}"\nprintf "arg=%s\\n" "$@"\n')
        mock.chmod(0o755)

    def launch(self, trace=None, inherited=None):
        env = {**os.environ, 'PATH':str(self.root)+':'+os.environ['PATH'], 'D':str(self.root)}
        env.pop('WAYLAND_DEBUG', None);env.pop('ALPINE_KONSOLE_INPUT_TRACE', None)
        if trace is not None:env['ALPINE_KONSOLE_INPUT_TRACE'] = trace
        if inherited is not None:env['WAYLAND_DEBUG'] = inherited
        command = self.block+'wait "$client"\nprintf "client=%s parent=%s\\n" "$client" "${WAYLAND_DEBUG-unset}"\ncat "$D/konsole.log"\n'
        return subprocess.run(['/bin/sh', '-eu', '-c', command], env=env, text=True, capture_output=True, timeout=5)

    def test_disabled_normal_launch_and_existing_environment(self):
        for trace in [None, '0', 'false', '2']:
            for inherited in [None, 'server']:
                with self.subTest(trace=trace, inherited=inherited):
                    r=self.launch(trace, inherited);self.assertEqual(r.returncode,0,r.stderr)
                    first,child,*args=r.stdout.splitlines()
                    client=first.split()[0].split('=')[1]
                    self.assertEqual(child, 'pid='+client+' debug='+(inherited or 'unset'))
                    self.assertEqual(first.split()[1], 'parent='+(inherited or 'unset'))
                    self.assertIn('arg=--separate',args);self.assertIn('arg=--nofork',args)

    def test_trace_only_changes_child_environment_and_keeps_pid(self):
        for inherited in [None, 'server']:
            r=self.launch('1',inherited);self.assertEqual(r.returncode,0,r.stderr)
            first,child,*_=r.stdout.splitlines();client=first.split()[0].split('=')[1]
            self.assertEqual(child,'pid='+client+' debug=client')
            self.assertEqual(first.split()[1], 'parent='+(inherited or 'unset'))
            plain=self.launch(None,inherited)
            self.assertEqual(r.stdout.splitlines()[2:],plain.stdout.splitlines()[2:])

    def test_finish_trace_is_opt_in_and_filters_only_keyboard_lines(self):
        start=self.source.index(' if [ "${ALPINE_KONSOLE_INPUT_TRACE:-0}" = 1 ];then\n',self.source.index('finish()'))
        end=self.source.index(' fi\n',start)+len(' fi\n');block=self.source[start:end]
        (self.root/'konsole.log').write_text('other message\n[1.000] {Default Queue} wl_keyboard#26.key(1, 2, 3, 1)\n')
        for trace in ['0','1']:
            r=subprocess.run(['/bin/sh','-eu','-c',block],env={**os.environ,'D':str(self.root),'ALPINE_KONSOLE_INPUT_TRACE':trace},capture_output=True,text=True,timeout=5)
            self.assertEqual(r.returncode,0,r.stderr)
            if trace=='0':self.assertEqual(r.stdout,'')
            else:
                self.assertEqual(r.stdout,'INNER_KEYBOARD_DISPATCH_TRACE_BEGIN\n[1.000] {Default Queue} wl_keyboard#26.key(1, 2, 3, 1)\nINNER_KEYBOARD_DISPATCH_TRACE_END\n')
        (self.root/'konsole.log').unlink()
        r=subprocess.run(['/bin/sh','-eu','-c',block],env={**os.environ,'D':str(self.root),'ALPINE_KONSOLE_INPUT_TRACE':'1'},capture_output=True,text=True,timeout=5)
        self.assertEqual(r.returncode,0);self.assertIn('TRACE_END',r.stdout)


if __name__=='__main__':
    unittest.main()
