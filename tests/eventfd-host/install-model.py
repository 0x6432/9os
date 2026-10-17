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
p=Path(a[a.index('-o')+1]);stem=Path(source).stem
expected={'qt-key-observer':'lib9os-qt-key-observer.so','qt-key-observer-mock':'libqt-key-observer-mock.so'}.get(stem,stem)
assert p.name==expected
if stem in ['qt-key-observer','qt-key-observer-mock']:
 assert '-shared' in a and '-fPIC' in a and '-Werror' in a and '-D_FORTIFY_SOURCE=2' in a
 assert '-Wl,--version-script=userland/tests/qt-key-observer/'+stem+'.map' in a
if stem=='qt-key-observer-caller':
 assert '-lqt-key-observer-mock' in a and '-Wl,-rpath,/usr/lib/9os-tests' in a
p.write_text('fake compiler output\\n')
with open(os.environ['INSTALL_RECORD'],'a') as f:f.write(source+'\\n')
'''
def run(text, consumer='eventfdwaketest'):
 with tempfile.TemporaryDirectory(prefix='9os-eventfd-install-') as temp:
  d=Path(temp);(d/'scripts').mkdir();(d/'bin').mkdir()
  (d/'userland/alpine-root-x86_64/usr/bin').mkdir(parents=True)
  p=d/'scripts/install.sh';p.write_text(text)
  cc=d/'bin/clang';cc.write_text(compiler);cc.chmod(0o755)
  record=d/'record';env=dict(os.environ,PATH=str(d/'bin')+os.pathsep+os.environ['PATH'],INSTALL_RECORD=str(record))
  subprocess.run(['sh',str(p),'x86_64'],check=True,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  calls=record.read_text().splitlines()
  output={'qt-key-observer':'usr/lib/lib9os-qt-key-observer.so','qt-key-observer-mock':'usr/lib/9os-tests/libqt-key-observer-mock.so'}.get(consumer,'usr/bin/'+consumer)
  source=f'userland/tests/qt-key-observer/{consumer}.c' if consumer.startswith('qt-key-observer') else f'userland/tests/{consumer}.c'
  return calls.count(source)==1 and (d/'userland/alpine-root-x86_64'/output).is_file()
assert run(script), 'new consumer was not installed exactly once'
for label,changed in [('missing',script.replace(' eventfdwaketest ', ' ',1)),('duplicate',script.replace(' eventfdwaketest ',' eventfdwaketest eventfdwaketest ',1))]:
 assert changed!=script
 assert not run(changed), 'installer mutation survived: '+label
print('eventfd installer model: x86 path passed; missing/duplicate mutations rejected')

assert run(script, 'pollfiltertest'), 'poll filter consumer not installed exactly once'
for label,changed in [('missing',script.replace(' pollfiltertest ', ' ',1)),('duplicate',script.replace(' pollfiltertest ',' pollfiltertest pollfiltertest ',1))]:
 assert changed!=script
 assert not run(changed, 'pollfiltertest'), 'poll installer mutation survived: '+label
print('poll filter installer model: x86 path passed; missing/duplicate mutations rejected')

assert run(script, 'evdevframetest'), 'evdev consumer not installed exactly once'
for label,changed in [('missing',script.replace(' evdevframetest ', ' ',1)),('duplicate',script.replace(' evdevframetest ',' evdevframetest evdevframetest ',1))]:
 assert changed!=script
 assert not run(changed, 'evdevframetest'), 'evdev installer mutation survived: '+label
print('evdev installer model: x86 path passed; missing/duplicate mutations rejected')

# Exercise the new shared-library path rather than silently ignoring it. Keep
# every earlier eventfd/poll/evdev missing/duplicate negative unchanged.
for consumer,token in [('qt-key-observer','observer'),('qt-key-observer-mock','mock')]:
 assert run(script,consumer), 'Qt diagnostic shared library not installed exactly once: '+consumer
 for label,changed in [('missing',script.replace('for model in observer mock;do','for model in '+('mock'if token=='observer'else'observer')+';do',1)),('duplicate',script.replace('for model in observer mock;do','for model in observer mock '+token+';do',1))]:
  assert changed!=script
  assert not run(changed,consumer), 'Qt library installer mutation survived: '+consumer+' '+label
assert run(script,'qt-key-observer-caller'), 'ABI surrogate caller not installed exactly once'
print('Qt observer installer model: libraries/caller paths and strict flags passed; missing/duplicate libraries rejected')

# mkroot treats every top-level tests/*.c as a standalone executable. Keep the
# shared libraries and their linked caller outside that namespace; the Alpine
# installer above builds all three explicitly. This catches the source-CI
# regression without building any non-x86 target locally.
fixtures=ROOT/'userland/tests/qt-key-observer'
for name in ['qt-key-observer','qt-key-observer-mock','qt-key-observer-caller']:
 assert (fixtures/(name+'.c')).is_file(), 'missing nested ABI fixture: '+name
 assert not (ROOT/'userland/tests'/(name+'.c')).exists(), 'ABI fixture leaked into standalone executable loop: '+name
for name in ['qt-key-observer','qt-key-observer-mock']:
 assert (fixtures/(name+'.map')).is_file()
assert '"$TOP"/tests/*.c' in (ROOT/'userland/mkroot.sh').read_text()
print('Qt observer source isolation: standalone mkroot namespace excludes all ABI fixtures')
