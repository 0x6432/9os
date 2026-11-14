#!/usr/bin/env python3
"""Synthetic exact-dispatch controls, not hardware or real Qt delivery proof."""
from pathlib import Path
from importlib.machinery import SourceFileLoader
import unittest

M = SourceFileLoader('inner_dispatch', str(Path(__file__).with_name('inner-keyboard-dispatch-check.py'))).load_module()


def wrap(lines):
    return 'INNER_KEYBOARD_DISPATCH_TRACE_BEGIN\n' + '\n'.join(lines) + '\nINNER_KEYBOARD_DISPATCH_TRACE_END\n'


class Dispatch(unittest.TestCase):
    def setUp(self):
        expected = [edge for frame in M.CAPTURE.expected_frames(M.CAPTURE.DEFAULT_TEXT) for edge in frame]
        self.lines = ['[1234567.123] {Default Queue} wl_keyboard#26.repeat_info(25, 600)'] + [
            f'[1234567.123] {{Default Queue}} wl_keyboard#26.key({i}, 42, {code}, {state})' for i, (code, state) in enumerate(expected)]
        self.good = wrap(self.lines)

    def test_exact_and_identical_replay(self):
        self.assertEqual(M.check(self.good)['key_edges'], 262)
        self.assertEqual(M.check(self.good+self.good)['identical_log_replays'], 2)
        self.assertEqual(M.check(wrap(self.lines+[self.lines[0]]))['repeat_announcements'], 2)

    def test_bad_framing_and_event_sequences(self):
        variants = ['', self.good.rsplit('INNER_KEYBOARD_DISPATCH_TRACE_END', 1)[0], wrap(self.lines[:-1]),
                    wrap(self.lines+self.lines[-2:]), wrap(self.lines[:3]+list(reversed(self.lines[3:5]))+self.lines[5:]),
                    self.good+wrap(self.lines[:-1]), 'INNER_KEYBOARD_DISPATCH_TRACE_BEGIN\n'+self.good,
                    self.good+'INNER_KEYBOARD_DISPATCH_TRACE_END\n']
        for raw in variants:
            with self.subTest(raw=raw[:80]), self.assertRaises(ValueError):
                M.check(raw)

    def test_malformed_discarded_outgoing_or_mixed_keys(self):
        variants = [self.good.replace('wl_keyboard#26.key(', prefix+'wl_keyboard#26.key(', 1) for prefix in ['discarded ', ' -> ']]
        variants += [self.good.replace('.key(0, 42,', '.key(0, broken,', 1),
                     self.good.replace('wl_keyboard#26.key(', 'wl_keyboard#27.key(', 1),
                     self.good.replace('{Default Queue} wl_keyboard#26.key(', '{Other Queue} wl_keyboard#26.key(', 1)]
        for raw in variants:
            with self.subTest(raw=raw[:80]), self.assertRaises(ValueError):
                M.check(raw)

    def test_unsigned_clock_and_signed_repeat_bounds(self):
        variants = [self.good.replace('.key(0, 42,', '.key(0, 4294967296,', 1),
                    self.good.replace('1234567.123', '4294967.296'),
                    self.good.replace('(25, 600)', '(2147483648, 600)'),
                    self.good.replace('(25, 600)', '(25, -2147483649)')]
        for raw in variants:
            with self.subTest(raw=raw[:80]), self.assertRaises(ValueError):
                M.check(raw)
        # A wrapping realtime clock is legitimate. Sequence checker must not
        # invent a monotonic or elapsed-time guarantee from the debug prefix.
        valid = self.good.replace('[1234567.123] {Default Queue} wl_keyboard#26.key(0,', '[4294967.295] {Default Queue} wl_keyboard#26.key(0,', 1)
        self.assertEqual(M.check(valid)['key_edges'], 262)

    def test_repeat_configuration_and_identity(self):
        variants = [wrap(self.lines[1:]), wrap(self.lines+[self.lines[0].replace('(25, 600)', '(25, 700)')]),
                    self.good.replace('wl_keyboard#26.repeat_info', 'wl_keyboard#27.repeat_info'),
                    self.good.replace('{Default Queue} wl_keyboard#26.repeat_info', '{Other Queue} wl_keyboard#26.repeat_info')]
        for raw in variants:
            with self.subTest(raw=raw[:80]), self.assertRaises(ValueError):
                M.check(raw)


if __name__ == '__main__':
    unittest.main()
