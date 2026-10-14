#!/usr/bin/env python3
"""Offline regressions for the real archive downloader (mock curl/sleep)."""
import gzip, os, pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as td:
    d = pathlib.Path(td); tools = d / 'bin'; tools.mkdir()
    fixture = d / 'fixture.gz'; fixture.write_bytes(gzip.compress(b'9os archive fixture'))
    count = d / 'count'; out = d / 'download.gz'
    (tools / 'curl').write_text('''#!/usr/bin/env python3
import os,pathlib,sys
args=sys.argv[1:]
assert '--retry-all-errors' in args and '--retry-max-time' in args
out=pathlib.Path(args[args.index('-o')+1]); final=pathlib.Path(os.environ['FETCH_FINAL'])
assert not final.exists(), 'partial data must not be published as the cached file'
p=pathlib.Path(os.environ['FETCH_COUNT']); n=int(p.read_text())+1 if p.exists() else 1; p.write_text(str(n))
mode=os.environ['FETCH_MODE']
if mode=='fail' or (mode=='once' and n==1): out.write_bytes(b'truncated'); sys.exit(6)
if mode=='html': out.write_text('<html>upstream error</html>'); sys.exit(0)
out.write_bytes(pathlib.Path(os.environ['FETCH_FIXTURE']).read_bytes())
''')
    (tools / 'sleep').write_text('#!/bin/sh\nexit 0\n')
    for p in tools.iterdir(): p.chmod(0o755)
    env = dict(os.environ, PATH=str(tools)+':'+os.environ['PATH'], FETCH_FINAL=str(out), FETCH_COUNT=str(count), FETCH_FIXTURE=str(fixture))
    def run(mode, success):
        env['FETCH_MODE'] = mode
        p = subprocess.run([str(root/'userland/fetch.sh'), str(out), 'https://example.invalid/archive.gz'], env=env, capture_output=True, timeout=15)
        assert (p.returncode==0)==success, p.stderr.decode()
        assert not pathlib.Path(str(out)+'.part').exists()
    run('once', True)
    assert count.read_text()=='2' and out.read_bytes()==fixture.read_bytes()
    run('fail', True) # A valid cache must not invoke curl.
    assert count.read_text()=='2'
    out.write_text('<html>poisoned cache</html>'); count.unlink()
    run('ok', True)
    assert count.read_text()=='1' and out.read_bytes()==fixture.read_bytes()
    out.unlink(); count.unlink(); run('html', False)
    assert not out.exists() and count.read_text()=='3'
    count.unlink(); run('fail', False)
    assert not out.exists() and count.read_text()=='3'
print('fetch-host: passed (DNS retry, atomic cache, reuse, poisoned cache, HTML/partial rejection, cleanup)')
