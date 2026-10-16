#!/usr/bin/python3
"""Wire/FD transport controls, NOT GUI acceptance; same source on Linux and 9os."""
import array
import importlib.machinery
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest

HELPER = Path(__file__).resolve().parents[2] / 'bin/wayland-presentation-proxy'
proxy = importlib.machinery.SourceFileLoader('presentation_proxy', str(HELPER)).load_module()


def message(obj, opcode, payload=b''):
    return struct.pack('=II', obj, (8 + len(payload)) << 16 | opcode) + payload


def global_event(registry=7, name=b'wp_presentation\0'):
    padded = name + bytes((-len(name)) % 4)
    return message(registry, 0, struct.pack('=II', 42, len(name)) + padded + struct.pack('=I', 1))


def receive(sock, size):
    out = bytearray()
    while len(out) < size:
        part = sock.recv(size - len(out))
        if not part:
            raise RuntimeError('unexpected EOF')
        out.extend(part)
    return bytes(out)


class Controls(unittest.TestCase):
    def state(self, hide=True):
        state = proxy.RegistryFilter(hide)
        self.assertFalse(state.drop(message(1, 1, struct.pack('=I', 7)), True))
        return state

    def test_exact_optional_global_only(self):
        state = self.state()
        self.assertTrue(state.drop(global_event(), False))
        self.assertEqual(state.hidden, 1)
        for name in (b'wl_shm\0', b'wp_presentation_extra\0', b'xp_presentation\0'):
            self.assertFalse(state.drop(global_event(name=name), False))

    def test_hidden_global_removal_and_registry_lifetime(self):
        state = self.state()
        hidden_remove = message(7, 1, struct.pack('=I', 42))
        other_remove = message(7, 1, struct.pack('=I', 99))
        self.assertTrue(state.drop(global_event(), False))
        self.assertFalse(state.drop(other_remove, False))
        self.assertTrue(state.drop(hidden_remove, False))
        self.assertFalse(state.drop(hidden_remove, False))
        self.assertTrue(state.drop(global_event(), False))
        state.drop(message(1, 1, struct.pack('=I', 7)), False)
        state.drop(message(1, 1, struct.pack('=I', 7)), True)
        self.assertFalse(state.drop(hidden_remove, False))

    def test_client_payload_never_filtered(self):
        self.assertFalse(self.state().drop(global_event(), True))

    def test_unrelated_object_never_filtered(self):
        self.assertFalse(self.state().drop(global_event(99), False))

    def test_registry_id_reuse(self):
        state = self.state()
        state.drop(message(1, 1, struct.pack('=I', 7)), False)
        self.assertFalse(state.drop(global_event(), False))
        state.drop(message(1, 1, struct.pack('=I', 7)), True)
        self.assertTrue(state.drop(global_event(), False))

    def test_passthrough_control(self):
        state = self.state(False)
        self.assertFalse(state.drop(global_event(), False))
        self.assertEqual(state.hidden, 0)

    def test_malformed_string_not_filtered(self):
        data = bytearray(global_event())
        struct.pack_into('=I', data, 12, 4096)
        self.assertFalse(self.state().drop(data, False))

    def transport(self, fragments, expected, client=False, descriptor=None, short=False):
        sender, source = socket.socketpair()
        target, reader = socket.socketpair()
        reader.settimeout(5)
        errors = []
        class ShortWriter:
            def sendmsg(self, parts, controls):
                return target.sendmsg([parts[0][:5]], controls)
            def send(self, data):
                return target.send(data[:7])
            def shutdown(self, how):
                return target.shutdown(how)
        writer = ShortWriter() if short else target
        thread = threading.Thread(target=proxy.relay, args=(source, writer, self.state(), client, errors.append))
        thread.start()
        got_fds = []
        try:
            for i, part in enumerate(fragments):
                if descriptor is not None and i == 0:
                    sender.sendmsg([part], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [descriptor]))])
                else:
                    sender.sendall(part)
            sender.shutdown(socket.SHUT_WR)
            out = bytearray()
            while True:
                data, controls, flags, _ = reader.recvmsg(65536, socket.CMSG_SPACE(256 * 4))
                self.assertFalse(flags & socket.MSG_CTRUNC)
                for level, kind, raw in controls:
                    if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                        a = array.array('i'); a.frombytes(raw); got_fds.extend(a)
                if not data:
                    break
                out.extend(data)
            thread.join(5)
            self.assertFalse(thread.is_alive())
            self.assertEqual(bytes(out), expected)
            return errors, got_fds
        except BaseException:
            for fd in got_fds:
                os.close(fd)
            raise
        finally:
            for s in (sender, source, target, reader):
                s.close()

    def test_fragmented_headers_payload_and_coalesced_globals(self):
        good = global_event(name=b'wl_shm\0')
        raw = global_event() + good + global_event(99)
        errors, fds = self.transport([raw[i:i + 1] for i in range(len(raw))], good + global_event(99))
        self.assertEqual(errors, [])
        self.assertEqual(fds, [])

    def test_rights_survive_filtered_and_fragmented_messages(self):
        fd = os.memfd_create('proxy-control', 0)
        try:
            os.write(fd, b'real descriptor payload')
            os.lseek(fd, 0, os.SEEK_SET)
            good = message(90, 2, struct.pack('=I', 123))
            raw = global_event() + good
            errors, fds = self.transport([raw[:3], raw[3:]], good, descriptor=fd)
            try:
                self.assertEqual(errors, [])
                self.assertEqual(len(fds), 1)
                self.assertEqual(os.read(fds[0], 128), b'real descriptor payload')
            finally:
                for received in fds:
                    os.close(received)
        finally:
            os.close(fd)

    def test_client_direction_bytes_unchanged(self):
        raw = message(1, 1, struct.pack('=I', 19)) + message(99, 0, bytes(128))
        errors, fds = self.transport([raw[:4], raw[4:]], raw, client=True)
        self.assertEqual((errors, fds), ([], []))

    def test_short_writes_transfer_rights_exactly_once(self):
        fd = os.memfd_create('proxy-short-write', 0)
        try:
            raw = message(90, 2, bytes(256))
            errors, fds = self.transport([raw], raw, descriptor=fd, short=True)
            try:
                self.assertEqual(errors, [])
                self.assertEqual(len(fds), 1)
            finally:
                for received in fds:
                    os.close(received)
        finally:
            os.close(fd)

    def test_large_fragmented_message(self):
        raw = message(80, 2, bytes(65520))
        errors, fds = self.transport([raw[:8], raw[8:17000], raw[17000:]], raw)
        self.assertEqual((errors, fds), ([], []))

    def test_signal_forwarding_and_child_status(self):
        with tempfile.TemporaryDirectory(prefix='proxy-signal-') as directory:
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            path = directory + '/outer'
            listener.bind(path); listener.listen(1); listener.settimeout(5)
            code = "import os,socket,signal,sys;signal.signal(signal.SIGTERM,lambda *_:sys.exit(42));s=socket.socket(fileno=int(os.environ['WAYLAND_SOCKET']));s.sendall(bytes.fromhex('0100000000000800'));signal.pause()"
            process = subprocess.Popen([sys.executable, str(HELPER), '--', sys.executable, '-c', code],
                                       env=dict(os.environ, WAYLAND_DISPLAY=path),
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            connection, _ = listener.accept()
            try:
                self.assertEqual(receive(connection, 8), message(1, 0))
                process.send_signal(signal.SIGTERM)
                stdout, stderr = process.communicate(timeout=10)
                self.assertEqual(process.returncode, 42, (stdout, stderr))
                self.assertIn(b'child=42', stderr)
            finally:
                connection.close(); listener.close()
                if process.poll() is None:
                    process.kill(); process.wait()

    def test_invalid_size_rejected(self):
        errors, fds = self.transport([struct.pack('=II', 1, 7 << 16)], b'')
        self.assertEqual(len(errors), 1)
        self.assertEqual(fds, [])

    def test_truncated_message_rejected(self):
        errors, _ = self.transport([message(9, 0, bytes(8))[:-1]], b'')
        self.assertEqual(len(errors), 1)

    def test_real_exec_socket_and_exit_status(self):
        with tempfile.TemporaryDirectory(prefix='proxy-control-') as directory:
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            path = directory + '/outer'
            listener.bind(path); listener.listen(1); listener.settimeout(5)
            code = '''import os,socket,struct,sys
s=socket.socket(fileno=int(os.environ['WAYLAND_SOCKET']))
s.sendall(struct.pack('=III',1,(12<<16)|1,7))
b=bytearray()
while len(b)<28:
 p=s.recv(28-len(b))
 if not p:sys.exit(92)
 b.extend(p)
sys.exit(23 if b[16:23]==b'wl_shm\\0' else 91)
'''
            process = subprocess.Popen([sys.executable, str(HELPER), '--', sys.executable, '-c', code],
                                       env=dict(os.environ, WAYLAND_DISPLAY=path),
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            connection, _ = listener.accept()
            try:
                self.assertEqual(receive(connection, 12), message(1, 1, struct.pack('=I', 7)))
                connection.sendall(global_event() + global_event(name=b'wl_shm\0'))
                stdout, stderr = process.communicate(timeout=10)
                self.assertEqual(process.returncode, 23, (stdout, stderr))
                self.assertIn(b'hidden=1 child=23', stderr)
            finally:
                connection.close(); listener.close()
                if process.poll() is None:
                    process.kill(); process.wait()


if __name__ == '__main__':
    signal.alarm(60)
    unittest.main(verbosity=2)
