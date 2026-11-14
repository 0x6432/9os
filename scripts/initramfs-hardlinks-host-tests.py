#!/usr/bin/env python3
"""Host-only packing controls; no kernel execution or guest boot."""
import os, pathlib, stat, subprocess, tempfile, unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/initramfs-pack.sh'

def parse(data):
    pos, entries = 0, {}
    while True:
        header = data[pos:pos + 110]
        assert header[:6] == b'070701'
        fields = [int(header[6 + i * 8:14 + i * 8], 16) for i in range(13)]
        nsize, size = fields[11], fields[6]
        raw = data[pos + 110:pos + 110 + nsize]
        assert raw.endswith(b'\0')
        name = raw[:-1].decode()
        start = (pos + 110 + nsize + 3) & ~3
        if name == 'TRAILER!!!': return entries
        entries[name] = (fields, data[start:start + size])
        pos = (start + size + 3) & ~3

class Pack(unittest.TestCase):
    def test_independent_payloads_and_symlink(self):
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d); root = p/'source'; root.mkdir(); (root/'dir').mkdir()
            a=root/'dir/a'; a.write_bytes(b'one payload\n'); a.chmod(0o751)
            os.link(a,root/'dir/b'); os.symlink('a',root/'dir/link')
            out=p/'archive'; subprocess.run([str(SCRIPT),str(root),str(out)],check=True)
            entries=parse(out.read_bytes())
            for name in ('dir/a','dir/b'):
                fields, payload=entries[name]
                self.assertEqual(payload,b'one payload\n'); self.assertEqual(fields[4],1)
                self.assertEqual(fields[1] & 0o7777,0o751); self.assertEqual(fields[2:4],[0,0])
            fields,payload=entries['dir/link']
            self.assertTrue(stat.S_ISLNK(fields[1])); self.assertEqual(payload,b'a')
            self.assertEqual(os.stat(a).st_ino,os.stat(root/'dir/b').st_ino)
    def test_failed_cpio_keeps_existing_output(self):
        with tempfile.TemporaryDirectory() as d:
            p=pathlib.Path(d);root=p/'source';root.mkdir();(root/'file').write_bytes(b'data')
            out=p/'archive';out.write_bytes(b'unchanged')
            fake=p/'bin';fake.mkdir();cpio=fake/'cpio';cpio.write_text('#!/bin/sh\nexit 7\n');cpio.chmod(0o755)
            env=dict(os.environ,PATH=str(fake)+':'+os.environ['PATH'],TMPDIR=str(p))
            r=subprocess.run([str(SCRIPT),str(root),str(out)],capture_output=True,env=env)
            self.assertNotEqual(r.returncode,0);self.assertEqual(out.read_bytes(),b'unchanged')
            self.assertFalse(list(p.glob('archive.tmp.*')));self.assertFalse(list(p.glob('9os-initramfs.*')))
    def test_missing_source_does_not_replace_existing_output(self):
        with tempfile.TemporaryDirectory() as d:
            p=pathlib.Path(d);out=p/'archive';out.write_bytes(b'unchanged')
            r=subprocess.run([str(SCRIPT),str(p/'missing'),str(out)],capture_output=True)
            self.assertNotEqual(r.returncode,0);self.assertEqual(out.read_bytes(),b'unchanged')
            self.assertFalse(list(p.glob('archive.tmp.*')))

if __name__=='__main__':unittest.main()
