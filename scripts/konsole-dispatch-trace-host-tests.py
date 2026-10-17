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
        start = self.source.index(' konsole_command=konsole\n', self.source.index('export QT_LINUX_ACCESSIBILITY_ALWAYS_ON'))
        end = self.source.index(' pty_ready=0;', start)
        self.block = self.source[start:end]
        mock = self.root/'konsole'
        mock.write_text('#!/bin/sh\nprintf "pid=%s debug=%s\\n" "$$" "${WAYLAND_DEBUG-unset}"\nprintf "arg=%s\\n" "$@"\n')
        mock.chmod(0o755)

    def launch(self, trace=None, inherited=None, qt_trace=None):
        env = {**os.environ, 'PATH':str(self.root)+':'+os.environ['PATH'], 'D':str(self.root)}
        env.pop('WAYLAND_DEBUG', None);env.pop('ALPINE_KONSOLE_INPUT_TRACE', None);env.pop('ALPINE_QT_KEY_TRACE', None)
        if trace is not None:env['ALPINE_KONSOLE_INPUT_TRACE'] = trace
        if qt_trace is not None:env['ALPINE_QT_KEY_TRACE'] = qt_trace
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

    def test_independent_qt_submission_flag_selects_child_wrapper(self):
        mock=self.root/'konsole-key-trace'
        mock.write_text('#!/bin/sh\nprintf "submission-wrapper\\n"\nexec konsole "$@"\n');mock.chmod(0o755)
        for trace in [None,'0','1']:
            r=self.launch(trace,qt_trace='1');self.assertEqual(r.returncode,0,r.stderr)
            lines=r.stdout.splitlines();self.assertEqual(lines[1],'submission-wrapper')
            client=lines[0].split()[0].split('=')[1]
            self.assertEqual(lines[2],'pid='+client+' debug='+('client'if trace=='1'else'unset'))
        for flag in [None,'0','false','2']:
            r=self.launch(qt_trace=flag);self.assertEqual(r.returncode,0,r.stderr)
            self.assertNotIn('submission-wrapper',r.stdout)

    def test_qt_submission_frame_is_opt_in_and_filters(self):
        start=self.source.index(' if [ "${ALPINE_QT_KEY_TRACE:-0}" = 1 ];then\n',self.source.index('finish()'))
        end=self.source.index(' fi\n',start)+len(' fi\n');block=self.source[start:end]
        (self.root/'konsole.log').write_text('other message\nQT_KEY_TRACE_LOADED witness\nQT_KEY_TRACE seq=0 phase=begin\n')
        for flag in ['0','1']:
            r=subprocess.run(['/bin/sh','-eu','-c',block],env={**os.environ,'D':str(self.root),'ALPINE_QT_KEY_TRACE':flag},capture_output=True,text=True,timeout=5)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertEqual(r.stdout,''if flag=='0'else'QT_PLATFORM_KEY_TRACE_BEGIN\nQT_KEY_TRACE_LOADED witness\nQT_KEY_TRACE seq=0 phase=begin\nQT_PLATFORM_KEY_TRACE_END\n')
        (self.root/'konsole.log').unlink()
        r=subprocess.run(['/bin/sh','-eu','-c',block],env={**os.environ,'D':str(self.root),'ALPINE_QT_KEY_TRACE':'1'},capture_output=True,text=True,timeout=5)
        self.assertEqual(r.returncode,0);self.assertEqual(r.stdout,'QT_PLATFORM_KEY_TRACE_BEGIN\nQT_PLATFORM_KEY_TRACE_END\n')

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


    def test_outer_trace_frame_is_opt_in(self):
        start=self.source.index(' if [ "${ALPINE_PLASMA_KEY_TRACE:-0}" = 1 ];then\n',self.source.index('finish()'))
        end=self.source.index(' fi\n',start)+len(' fi\n');block=self.source[start:end]
        (self.root/'kwin.log').write_text('unrelated\nWAYLAND_KEY_TRACE object=16 event=key received_ns=1 serial=1 time_ms=2 key=20 state=1\n')
        for trace in ['0','1']:
            r=subprocess.run(['/bin/sh','-eu','-c',block],env={**os.environ,'D':str(self.root),'ALPINE_PLASMA_KEY_TRACE':trace},capture_output=True,text=True,timeout=5)
            self.assertEqual(r.returncode,0,r.stderr)
            if trace=='0':self.assertEqual(r.stdout,'')
            else:
                self.assertIn('OUTER_KEYBOARD_TRACE_BEGIN\nWAYLAND_KEY_TRACE ',r.stdout)
                self.assertTrue(r.stdout.endswith('OUTER_KEYBOARD_TRACE_END\n'))
                self.assertNotIn('unrelated',r.stdout)


if __name__=='__main__':
    unittest.main()
