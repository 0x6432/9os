#!/usr/bin/env python3
"""Exercise the actual x86 Alpine tiny-consumer installer with a fake compiler.
No target binary or library is built; PR CI owns actual all-architecture builds.
"""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
script=(ROOT/'scripts/alpine-build-tests.sh').read_text()
compiler='''#!/usr/bin/env python3
import os,sys
from pathlib import Path
a=sys.argv[1:]
assert '--target=x86_64-linux-musl' in a
source=next(x for x in a if x.startswith('userland/tests/') and x.endswith('.c'))
p=Path(a[a.index('-o')+1]);assert p.name==Path(source).stem
p.write_text('fake compiler output\\n')
with open(os.environ['INSTALL_RECORD'],'a') as f:f.write(source+'\\n')
'''
def run(text):
 with tempfile.TemporaryDirectory(prefix='9os-eventfd-install-') as temp:
  d=Path(temp);(d/'scripts').mkdir();(d/'bin').mkdir()
  (d/'userland/alpine-root-x86_64/usr/bin').mkdir(parents=True)
  p=d/'scripts/install.sh';p.write_text(text)
  cc=d/'bin/clang';cc.write_text(compiler);cc.chmod(0o755)
  record=d/'record';env=dict(os.environ,PATH=str(d/'bin')+os.pathsep+os.environ['PATH'],INSTALL_RECORD=str(record))
  subprocess.run(['sh',str(p),'x86_64'],check=True,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  calls=record.read_text().splitlines()
  return calls.count('userland/tests/eventfdwaketest.c')==1 and (d/'userland/alpine-root-x86_64/usr/bin/eventfdwaketest').is_file()
assert run(script), 'new consumer was not installed exactly once'
for label,changed in [('missing',script.replace(' eventfdwaketest ', ' ',1)),('duplicate',script.replace(' eventfdwaketest ',' eventfdwaketest eventfdwaketest ',1))]:
 assert changed!=script
 assert not run(changed), 'installer mutation survived: '+label
print('eventfd installer model: x86 path passed; missing/duplicate mutations rejected')
