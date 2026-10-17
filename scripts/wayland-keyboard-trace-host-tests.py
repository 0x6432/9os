#!/usr/bin/env python3
"""Synthetic wire observation controls, NOT actual keyboard or desktop proof."""
import importlib.machinery
from pathlib import Path
import struct
import os
import unittest

proxy = importlib.machinery.SourceFileLoader('key_trace_proxy', str(Path(__file__).resolve().parents[1]/'userland/alpine/desktop-overlay/usr/bin/wayland-presentation-proxy')).load_module()


def msg(obj, opcode, payload=b''):
    return struct.pack('=II', obj, (len(payload)+8)<<16 | opcode)+payload


def bind(registry, interface, new_id):
    name=interface.encode()+b'\0';pad=name+b'\0'*((-len(name))%4)
    return msg(registry, 0, struct.pack('=II',42,len(name))+pad+struct.pack('=II',7,new_id))


class Trace(unittest.TestCase):
    def setUp(self):
        self.lines=[]
        self.trace=proxy.KeyboardTrace(self.lines.append)
        self.state=proxy.RegistryFilter(keyboard_trace=self.trace)
        self.send(msg(1,1,struct.pack('=I',7)), True)
        self.send(bind(7,'wl_seat',8), True)
        self.send(msg(8,1,struct.pack('=I',9)), True)

    def send(self, message, client=False):
        self.assertFalse(self.state.drop(message,client))

    def test_key_and_repeat_payloads_exact(self):
        self.send(msg(9,5,struct.pack('=ii',25,600)))
        self.send(msg(9,3,struct.pack('=IIII',123,789,20,1)))
        self.send(msg(9,3,struct.pack('=IIII',124,789,20,0)))
        self.assertEqual(len(self.lines),3)
        self.assertRegex(self.lines[0],r'object=9 event=repeat_info received_ns=\d+ rate=25 delay=600$')
        self.assertRegex(self.lines[1],r'object=9 event=key received_ns=\d+ serial=123 time_ms=789 key=20 state=1$')
        self.assertTrue(self.lines[2].endswith('key=20 state=0'))

    def test_only_tracked_server_keyboard(self):
        key=struct.pack('=IIII',1,2,3,1)
        self.send(msg(10,3,key));self.send(msg(9,3,key),True)
        self.send(bind(7,'wl_seat_extra',11),True)
        self.send(msg(11,1,struct.pack('=I',12)),True)
        self.send(msg(12,3,key))
        self.assertEqual(self.lines,[])

    def test_lifetime_does_not_reuse_keyboard_id(self):
        self.send(msg(1,1,struct.pack('=I',9)))
        self.send(msg(9,3,struct.pack('=IIII',1,2,3,1)))
        self.assertEqual(self.lines,[])
        self.send(msg(8,1,struct.pack('=I',9)),True)
        self.send(msg(9,0),True)
        self.send(msg(9,3,struct.pack('=IIII',1,2,3,1)))
        self.assertEqual(self.lines,[])

    def test_enter_leave_and_invalid_length(self):
        self.send(msg(9,1,struct.pack('=IIII',1,20,4,42)))
        self.send(msg(9,2,struct.pack('=II',2,20)))
        self.send(msg(9,1,struct.pack('=III',3,20,100)))
        self.assertEqual(len(self.lines),2)
        self.assertTrue(self.lines[0].endswith('held_keys=1'))
        self.assertIn('event=leave',self.lines[1])

    def test_bounded_log(self):
        for _ in range(4100):self.send(msg(9,3,struct.pack('=IIII',1,2,3,1)))
        self.assertEqual(len(self.lines),4097)
        self.assertEqual(self.lines[-1],'WAYLAND_KEY_TRACE truncated=1')

    def test_traced_fragmented_wire_and_rights_unchanged(self):
        path=Path(__file__).resolve().parents[1]/'userland/alpine/desktop-overlay/usr/share/9os/wayland-proxy-selftest.py'
        controls=importlib.machinery.SourceFileLoader('trace_wire_controls',str(path)).load_module()
        wire=controls.Controls()
        wire.state=lambda *args:self.state
        raw=msg(9,3,struct.pack('=IIII',7,8,20,1))+msg(9,3,struct.pack('=IIII',8,8,20,0))
        fd=os.memfd_create('trace-fd',0)
        try:
            os.write(fd,b'unchanged trace descriptor');os.lseek(fd,0,os.SEEK_SET)
            errors,fds=wire.transport([raw[:3],raw[3:11],raw[11:]],raw,descriptor=fd,short=True)
            try:
                self.assertEqual(errors,[]);self.assertEqual(len(fds),1)
                self.assertEqual(os.read(fds[0],100),b'unchanged trace descriptor')
                self.assertEqual(len(self.lines),2)
            finally:
                for got in fds:os.close(got)
        finally:os.close(fd)

    def test_normal_mode_has_no_trace(self):
        state=proxy.RegistryFilter()
        self.assertIsNone(state.keyboard_trace)
        self.assertFalse(state.drop(msg(9,3,struct.pack('=IIII',1,2,3,1)),False))


if __name__=='__main__':unittest.main()
