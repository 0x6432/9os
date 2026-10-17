#!/usr/bin/env python3
"""Synthetic parser rejection controls, NOT physical keyboard evidence."""
import importlib.machinery
from pathlib import Path
import unittest

module = importlib.machinery.SourceFileLoader('input_capture_check', str(Path(__file__).with_name('input-capture-check.py'))).load_module()


def log(frames=None, done=None, header=None):
    frames = module.expected_frames(module.DEFAULT_TEXT) if frames is None else frames
    out = [header or 'INPUT_CAPTURE_DEVICE index=1 named=1 monotonic=1 name=QEMU Virtio Keyboard', 'INPUT_CAPTURE_READY devices=1']
    rows = []
    for index, frame in enumerate(frames):
        for code, value in frame:
            rows.append((1, code, value, index))
        rows.append((0, 0, 0, index))
    for typ, code, value, index in rows:
        out.append('INPUT_CAPTURE_EVENT device=1 sec=10 usec=%d type=%d code=%d value=%d received_ns=%d' % (index, typ, code, value, 10000000000 + index * 1000))
    repeats = sum(typ == 1 and value == 2 for typ, code, value, _ in rows)
    out.append(done or 'INPUT_CAPTURE_DONE events=%d repeats=%d dropped=0 error=0' % (len(rows), repeats))
    return '\n'.join(out)


class Capture(unittest.TestCase):
    def test_known_qmp_linux_keycodes(self):
        self.assertEqual(module.expected_frames('t>/_\n'), [[(20,1),(20,0)],[(42,1),(52,1),(52,0),(42,0)],[(53,1),(53,0)],[(42,1),(12,1),(12,0),(42,0)],[(28,1),(28,0)]])
    def test_full_command(self):
        result=module.check(log());self.assertEqual(result['taps'],len(module.DEFAULT_TEXT));self.assertEqual(result['explicit_repeats'],0)
    def test_key_and_frame_corruption_rejected(self):
        original=module.expected_frames(module.DEFAULT_TEXT)
        variants=[original[:-1], [[(20,1),(20,1),(20,0)]]+original[1:], [[(20,1),(20,2),(20,0)]]+original[1:], [[(20,1)]]+original[1:], [[(20,1)],[(20,0)]]+original[1:]]
        for frames in variants:
            with self.subTest(frames=frames[:2]):
                with self.assertRaises(ValueError):module.check(log(frames))
    def test_incomplete_and_ambiguous_records_rejected(self):
        original=log()
        variants=[original.rsplit('\n',1)[0],original+'\n'+original.splitlines()[-1],original.replace('monotonic=1','monotonic=0'),original.replace('dropped=0','dropped=1'),original.replace('error=0','error=1'),original.replace('type=0 code=0','type=0 code=3',1),original.replace('sec=10 usec=1 ','sec=9 usec=1 ',1),original.replace('usec=0','usec=1000000',1),original.replace('device=1 sec=','device=9 sec=',1),original.replace('INPUT_CAPTURE_EVENT device=1 sec=10 usec=0','INPUT_CAPTURE_EVENT broken',1)]
        for raw in variants:
            with self.subTest(raw=raw[:80]):
                with self.assertRaises(ValueError):module.check(raw)


if __name__=='__main__':unittest.main()
