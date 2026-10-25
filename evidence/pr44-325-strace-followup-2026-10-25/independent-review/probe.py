import subprocess,pathlib,tempfile,os,json,re
root=pathlib.Path('/data/pr44-strace-prototype-review');cmd=(root/'candidate.txt').read_text();trace=pathlib.Path('/tmp/alpine-strace.log');assert not trace.exists(),'avoid replacing existing trace'
results=[]
try:
 payload=b'\x00\r\x1b__RC51=0\nFAIL\npanic\n#\n'+b'Z'*16000
 trace.write_bytes(payload)
 for shell in [['/bin/bash'],['/bin/bash','--posix']]:
  for rc,gc in [(137,0),(23,0),(1,0),(0,1),(0,0)]:
   with tempfile.TemporaryDirectory() as t:
    calls=pathlib.Path(t)/'calls'
    prefix=f'timeout(){{ echo timeout >>"{calls}"; return {rc}; }}; grep(){{ echo grep >>"{calls}"; return {gc}; }}; '
    p=subprocess.run(shell+['-c',prefix+cmd+'; echo __RC51=$?; echo alive'],capture_output=True)
    o=p.stdout; expected=rc if rc else gc
    assert re.findall(rb'__RC51=(\d+)\n',o)==[str(expected).encode()]
    assert b'FAIL' not in o and b'panic' not in o and b'\x1b' not in o and b'\x00' not in o
    assert p.stderr==b'' and o.endswith(b'alive\n')
    assert calls.read_text().splitlines()==(['timeout'] if rc else ['timeout','grep'])
    if expected:
     m=re.search(rb'ALPINE_STRACE_TAIL bytes=(\d+) truncated=(\d) hex=([0-9a-f]*)\n',o);assert m and m[1]==b'1024' and m[2]==b'1' and bytes.fromhex(m[3].decode())==payload[-1024:]
    else:assert o==b'__RC51=0\nalive\n'
    results.append({'shell':shell,'trace_rc':rc,'grep_rc':gc,'pass':True})
  # Complete valid od output then nonzero must be unknown, never accepted.
  for utility in ['od','tr']:
   prefix=f'timeout(){{ return 137; }}; {utility}(){{ command {utility} "$@"; return 9; }}; '
   p=subprocess.run(shell+['-c',prefix+cmd+'; echo __RC51=$?'],capture_output=True)
   assert p.stdout==b'ALPINE_STRACE_TAIL unknown\n__RC51=137\n' and p.stderr==b''
   results.append({'shell':shell,'valid_output_then_nonzero':utility,'pass':True})
  # Failure-only metadata with stdout closed: diagnose stderr scope.
  p=subprocess.run(shell+['-c','timeout(){ return 137; }; exec 1>&-; '+cmd],capture_output=True)
  results.append({'shell':shell,'closed_stdout_returncode':p.returncode,'closed_stdout_stderr':p.stderr.decode()})
finally:trace.unlink(missing_ok=True)
(root/'own-probe-results.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results[-1],indent=2));print('14 semantic normal/statusguard scenarios passed; 2 closedstdout probes recorded')
