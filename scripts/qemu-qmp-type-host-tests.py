#!/usr/bin/env python3
"""Mock batched-tap QMP controls, NOT physical guest delivery evidence."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

SOURCE = Path(os.environ.get('QEMU_TYPE_SOURCE', Path(__file__).with_name('qemu-type.py')))


class QMPType(unittest.TestCase):
    def run_keys(self, text, fail=False):
        with tempfile.TemporaryDirectory() as tmp:
            path = str(Path(tmp)/'qmp')
            messages = []
            with socket.socket(socket.AF_UNIX) as server:
                server.bind(path); server.listen(1); server.settimeout(10)
                def serve():
                    with server.accept()[0] as client:
                        client.sendall(b'{"QMP":{"version":{},"capabilities":[]}}\r\n')
                        f = client.makefile('rb')
                        while line := f.readline():
                            message = json.loads(line); messages.append(message)
                            response = {'id':message['id'], 'return':{}}
                            if fail and message['execute']=='input-send-event':
                                response = {'id':message['id'], 'error':{'class':'GenericError','desc':'mock failure'}}
                            # Asynchronous monitor events cannot be mistaken for an ack.
                            client.sendall(b'{"event":"MOCK"}\r\n'+json.dumps(response).encode()+b'\r\n')
                t = threading.Thread(target=serve);t.start()
                r = subprocess.run([sys.executable,str(SOURCE),'--qmp',path,text],capture_output=True,timeout=10)
                t.join(10);self.assertFalse(t.is_alive())
            self.assertEqual(r.returncode==0,not fail,r.stderr)
            self.assertEqual(messages[0]['execute'],'qmp_capabilities')
            return messages[1:]

    def test_distinct_repeated_down_up(self):
        messages = self.run_keys('tty\\n')
        self.assertEqual(len(messages),4)
        for index,key in enumerate(['t','t','y','ret']):
            for offset,down in enumerate([True,False]):
                event = [messages[index]['arguments']['events'][offset]]
                self.assertEqual(event,[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':key}}}])
        self.assertTrue(all(m['execute']=='input-send-event' for m in messages))
        self.assertTrue(all('hold-time' not in m['arguments'] for m in messages))

    def test_shift_released_in_reverse_order(self):
        messages = self.run_keys('A>')
        for index,key in enumerate(['a','dot']):
            events = messages[index]['arguments']['events']
            down, up = events[:2], events[2:]
            self.assertEqual([e['data']['key']['data'] for e in down],['shift',key])
            self.assertEqual([e['data']['key']['data'] for e in up],[key,'shift'])
            self.assertTrue(all(e['data']['down'] for e in down))
            self.assertTrue(all(not e['data']['down'] for e in up))

    def test_rejected_key_fails_without_command_retry(self):
        self.assertEqual(len(self.run_keys('tty',True)),1)


if __name__=='__main__':
    unittest.main()
