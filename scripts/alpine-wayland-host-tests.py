#!/usr/bin/env python3
import base64, importlib.util, io, pathlib, unittest
from PIL import Image
p=pathlib.Path(__file__).with_name('alpine-wayland-check.py')
s=importlib.util.spec_from_file_location('wayland_check',p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m)

def evidence(color=(17,170,85,255),size=(128,96),trace=None,uid=b'0'):
    b=io.BytesIO();Image.new('RGBA',size,color).save(b,format='PNG')
    if trace is None:
        trace=b''.join(b'wl_surface#30.frame(new id wl_callback#41)\nwl_callback#41.done(123)\n' for _ in range(3))
    return (b'ALPINE_WAYLAND_PNG_BEGIN\n'+base64.b64encode(b.getvalue())+b'\nALPINE_WAYLAND_PNG_END\n'+
            b'ALPINE_WAYLAND_TRACE_BEGIN uid='+uid+b'\nALPINE_WAYLAND_CLIENT_UID='+uid+b'\n'+trace+
            b'qml: alpine-wayland: READY frames 5 size 128 96\nALPINE_WAYLAND_TRACE_END\n')

class EvidenceTests(unittest.TestCase):
    def test_correct_root_and_nonroot_reused_ids(self):
        self.assertEqual(m.check(evidence()+evidence(uid=b'1000')),[3,3])
    def test_wrong_pixels_and_size(self):
        for kw in [dict(color=(0,0,0,255)),dict(size=(96,64))]:
            with self.subTest(kw=kw),self.assertRaises(ValueError):m.check(evidence(**kw))
    def test_sync_and_duplicate_done_not_frame_completions(self):
        for trace in [b'wl_callback#3.done(1)\n'*6,
                      b'wl_surface#30.frame(new id wl_callback#41)\n'+b'wl_callback#41.done(1)\n'*3]:
            with self.subTest(trace=trace),self.assertRaises(ValueError):m.check(evidence(trace=trace))
    def test_callbacks_cannot_be_borrowed_from_other_client(self):
        good=evidence();bad=evidence(trace=b'wl_callback#3.done(1)\n')
        with self.assertRaises(ValueError):m.check(good+bad)
    def test_uid_cannot_be_faked(self):
        with self.assertRaises(ValueError):m.check(evidence(uid=b'1000').replace(b'CLIENT_UID=1000',b'CLIENT_UID=0'))
    def test_ready_frame_count(self):
        for n in [3,10,29,100]:
            self.assertEqual(m.check(evidence().replace(b'frames 5',f'frames {n}'.encode())),[3])
        for n in [0,1,2]:
            with self.assertRaises(ValueError):m.check(evidence().replace(b'frames 5',f'frames {n}'.encode()))
    def test_missing_evidence(self):
        for raw in [b'',evidence().split(b'ALPINE_WAYLAND_TRACE_BEGIN')[0]]:
            with self.subTest(raw=raw),self.assertRaises(ValueError):m.check(raw)

if __name__=='__main__':unittest.main()
