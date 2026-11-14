#!/usr/bin/env python3
"""Mock-only controls for bounded introspection; no desktop evidence."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parent.parent
source=(ROOT/'userland/alpine/desktop-overlay/usr/bin/alpine-dolphin-probe').read_text()
function=source[source.index('find_window() {'):source.index('service=;object=;deadline=')]
with tempfile.TemporaryDirectory(prefix='9os-dolphin-discovery-') as tmp:
    d=Path(tmp);b=d/'bin';b.mkdir()
    (b/'timeout').write_text('#!/bin/sh\nshift 3\nexec "$@"\n')
    (b/'dbus-send').write_text('''#!/usr/bin/env python3
import os,sys
p=sys.argv[-2];m=os.environ['MOCK']
if m=='error':sys.exit(1)
if m=='oversize':
 if p=='/':print('<node>\\n'+'\\n'.join('<node name="n%d"/>'%i for i in range(40))+'\\n</node>')
 else:print('<node/>')
elif m=='deep':print('<node>\\n<node name="next"/>\\n</node>')
elif p=='/':print('<node>\\n<node name="MainApplication"/>\\n<node name="dolphin"/>\\n<node name="bad;name"/>\\n</node>')
elif p=='/dolphin':print('<node>\\n<node name="Dolphin_29"/>\\n</node>')
elif p=='/dolphin/Dolphin_29' and m=='valid':print('<node>\\n<interface name="org.kde.dolphin.MainWindow">\\n</interface>\\n</node>')
else:print('<node/>')
''')
    for x in b.iterdir():x.chmod(0o755)
    (d/'test.sh').write_text('set -eu\nD="$1"\n'+function+'\nfind_window org.kde.dolphin-42\n')
    for mode in ['valid','missing','error','oversize','deep']:
        p=subprocess.run(['sh',str(d/'test.sh'),tmp],env={**os.environ,'PATH':str(b)+':'+os.environ['PATH'],'MOCK':mode},capture_output=True,text=True,timeout=5)
        assert p.returncode==(0 if mode=='valid' else 1),(mode,p.stdout,p.stderr)
        assert p.stdout==('/dolphin/Dolphin_29\n' if mode=='valid' else ''),(mode,p.stdout)
        print('PASS',mode)
    print('5/5 bounded-discovery controls (mock only; no GUI claim)')

# Exercise the verbatim production URL-readiness helpers/loop with a persistent
# fake wall clock and fake D-Bus tools. ROOT is this repo copy, so `source` above
# always binds to the live candidate file being tested.
URL_GUARD = '   readback_now=$(date +%s) && [ "$readback_now" -lt "$deadline" ] || break\n'
assert URL_GUARD in source, 'candidate URL deadline guard missing from live source'
url_block = source[source.index('call_bool() {'):source.index('# isItemVisibleInAnyView')]


def put_exec(path, text):
    path.write_text(text)
    path.chmod(0o755)


def run_url_block(program, scenario):
    with tempfile.TemporaryDirectory(prefix='9os-dolphin-url-') as tmp:
        root=Path(tmp); b=root/'bin'; state=root/'state'; b.mkdir(); state.mkdir()
        (state/'clock').write_text('0'); (state/'methods').write_text('0'); (state/'date-calls').write_text('0')
        put_exec(b/'date', '''#!/usr/bin/env python3
import os,sys
from pathlib import Path
if sys.argv[1:] != ['+%s']: raise SystemExit(2)
state=Path(os.environ['FAKE_ROOT'],'state'); calls=state/'date-calls'
n=int(calls.read_text())+1; calls.write_text(str(n))
print(state.joinpath('clock').read_text().strip())
if os.environ['SCENARIO']=='date-output-failure' and n==3: raise SystemExit(1)
''')
        put_exec(b/'sleep', '''#!/usr/bin/env python3
import os
from pathlib import Path
r=Path(os.environ['FAKE_ROOT']); p=r/'state'/'clock'; n=int(p.read_text())
if os.environ['SCENARIO']=='transient-then-true':
    if n < 20: p.write_text('20')
elif n < 90: p.write_text('90')
''')
        put_exec(b/'timeout', '''#!/bin/sh
while [ "$#" -gt 0 ]; do
 case "$1" in
  -s|-k) shift 2 ;;
  --) shift; break ;;
  -*) shift ;;
  *) shift; break ;;
 esac
done
[ "$#" -gt 0 ] || exit 2
exec "$@"
''')
        put_exec(b/'strace', '#!/bin/sh\nexit 1\n')
        put_exec(b/'dbus-send', r'''#!/usr/bin/env python3
import os,sys
from pathlib import Path
root=Path(os.environ['FAKE_ROOT']); state=root/'state'; clock=state/'clock'; methods=state/'methods'
mode=os.environ['SCENARIO']; a=sys.argv[1:]
method=a[-2] if len(a)>2 and a[-2].endswith('org.kde.dolphin.MainWindow.isUrlOpen') else a[-1]
obj=a[-3] if method.endswith('org.kde.dolphin.MainWindow.isUrlOpen') else a[-2]
def now(): return int(clock.read_text())
def at(n): clock.write_text(str(n))
if method.endswith('org.freedesktop.DBus.ListNames'):
    if mode=='no-service': print(' string "org.freedesktop.DBus"'); raise SystemExit(0)
    if mode=='late-service': at(91)
    elif mode in ('true89','true90','true91','late-method','always-false','method-error','postfailure-diagnostic','date-output-failure'): at(89)
    elif mode=='transient-then-true' and now()<5: at(5)
    print(' string "org.kde.dolphin-42"')
elif method.endswith('org.freedesktop.DBus.Introspectable.Introspect'):
    if mode=='late-discovery' and obj=='/': at(91)
    if mode=='no-object':
        if obj=='/': print('<node>\n<node name="Other"/>\n</node>')
        else: print('<node/>')
    elif obj=='/': print('<node>\n<node name="dolphin"/>\n</node>')
    elif obj=='/dolphin': print('<node>\n<node name="Dolphin_1"/>\n</node>')
    elif obj=='/dolphin/Dolphin_1': print('<node>\n<interface name="org.kde.dolphin.MainWindow">\n</interface>\n</node>')
    else: print('<node/>')
elif method.endswith('org.kde.dolphin.MainWindow.isUrlOpen'):
    n=int(methods.read_text())+1; methods.write_text(str(n))
    if mode=='true89': at(89); print(' boolean true')
    elif mode=='true90': at(90); print(' boolean true')
    elif mode=='true91': at(91); print(' boolean true')
    elif mode=='date-output-failure': at(89); print(' boolean true')
    elif mode in ('late-service','late-discovery'): print(' boolean true')
    elif mode=='late-method': at(91); print(' boolean true')
    elif mode=='always-false': at(91); print(' boolean false')
    elif mode=='postfailure-diagnostic': at(90); print(' boolean false')
    elif mode=='method-error': at(91); print(' synthetic method error',file=sys.stderr); raise SystemExit(1)
    elif mode=='transient-then-true':
        if n==1: at(10); print(' synthetic transient D-Bus error',file=sys.stderr); raise SystemExit(1)
        at(30); print(' boolean true')
    else: raise SystemExit('unhandled scenario '+mode)
else: raise SystemExit('unhandled dbus-send method '+method)
''')
        put_exec(b/'qdbus', '''#!/usr/bin/env python3
import os,sys
mode=os.environ['SCENARIO']; a=sys.argv[1:]
if a==['org.kde.dolphin*']: print('org.kde.dolphin-42')
elif len(a)==1: print('/dolphin/Dolphin_1')
elif len(a)>=3 and a[2].endswith('.isUrlOpen'): print('true' if mode=='postfailure-diagnostic' else 'false')
''')
        harness='#!/bin/sh\nset -eu\nD='+shlex.quote(str(state))+'\nclient=$$\nQ='+shlex.quote(str(b/'qdbus'))+'\n'+program
        run=root/'run-url-block.sh'; run.write_text(harness)
        env={**os.environ,'PATH':str(b)+os.pathsep+os.environ['PATH'],'FAKE_ROOT':str(root),'SCENARIO':scenario}
        p=subprocess.run(['/bin/sh',str(run)],capture_output=True,text=True,env=env,timeout=10)
        return {'rc':p.returncode,'out':p.stdout,'err':p.stderr,'clock':int((state/'clock').read_text()),'method_calls':int((state/'methods').read_text())}


def require_ready(r):
    assert r['rc']==0,(r['rc'],r['out'],r['err'])
    assert 'DOLPHIN_URL_READY service=org.kde.dolphin-42 object=/dolphin/Dolphin_1' in r['out'],r
    assert 'alpine-dolphin: FAIL real directory URL readback' not in r['out'],r


def require_failed(r):
    assert r['rc']==1,(r['rc'],r['out'],r['err'])
    assert 'alpine-dolphin: FAIL real directory URL readback' in r['out'],r
    assert 'DOLPHIN_URL_READY' not in r['out'],r


# Candidate positive at t=89 and a transient method error followed by timely true.
require_ready(run_url_block(url_block,'true89'))
r=run_url_block(url_block,'transient-then-true'); require_ready(r); assert r['method_calls']==2,r
print('PASS URL readiness: true at t=89 accepted; transient D-Bus method error then timely true accepted (2 calls)')

# At t=90 the strict existing `< deadline` predicate rejects; t=91 and late
# service/object/method completions likewise remain failures.
for mode in ('true90','true91','late-service','late-discovery','late-method','always-false','no-service','no-object','method-error','postfailure-diagnostic','date-output-failure'):
    r=run_url_block(url_block,mode); require_failed(r)
    if mode=='postfailure-diagnostic':
        assert 'DOLPHIN_DIAGNOSTIC_OBJECT=/dolphin/Dolphin_1\ntrue\n' in r['out'],r
    print('PASS URL readiness:',mode,'rc=1','clock='+str(r['clock']))

# Baseline mutant: remove only the candidate guard from the live source. It
# reproduces the old in-flight acceptance; the production-policy assertion
# rejects it. No vendored source/original file or fixed source hash is used.
unguarded=source.replace(URL_GUARD,'',1)
for mode in ('true89','true90','true91','late-service','late-discovery','late-method','date-output-failure'):
    mutant=run_url_block(unguarded[unguarded.index('call_bool() {'):unguarded.index('# isItemVisibleInAnyView')],mode)
    require_ready(mutant)
    if mode!='true89':
        try: require_failed(mutant)
        except AssertionError: pass
        else: raise AssertionError('guard-removal mutant survived '+mode)
print('PASS mutation: guard-removal mutant is caught for t=90/t=91, late service/object/method, and date error')

# Boundary mutation: inclusive comparison must not accept t=90.
inclusive=source.replace(URL_GUARD,'   [ "$(date +%s)" -le "$deadline" ] || break\n',1)
inclusive_block=inclusive[inclusive.index('call_bool() {'):inclusive.index('# isItemVisibleInAnyView')]
inclusive_result=run_url_block(inclusive_block,'true90'); require_ready(inclusive_result)
try: require_failed(inclusive_result)
except AssertionError: pass
else: raise AssertionError('inclusive <= boundary mutant survived')
print('PASS mutation: <= boundary mutant is caught at t=90')

# Command substitution can print a numeric value and still exit nonzero. The old
# unchecked form accepts that value; the checked assignment must fail closed.
unchecked=source.replace(URL_GUARD,'   [ "$(date +%s)" -lt "$deadline" ] || break\n',1)
unchecked_block=unchecked[unchecked.index('call_bool() {'):unchecked.index('# isItemVisibleInAnyView')]
unchecked_result=run_url_block(unchecked_block,'date-output-failure'); require_ready(unchecked_result)
try: require_failed(unchecked_result)
except AssertionError: pass
else: raise AssertionError('unchecked date-status mutant survived')
print('PASS mutation: date output plus nonzero status is rejected by the checked guard')
print('PASS: URL deadline scenarios and mutants ran against the live candidate source')
