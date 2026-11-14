PYTHONDONTWRITEBYTECODE=1 python3 - <<'PY'
from pathlib import Path
import json,zipfile,re
p=Path('/data/pr42-current');repo=Path('/data/9os-team-integration')
for arch,zid in [('aarch64','12151762243'),('x86_64','12151532724'),('riscv64','12151722599')]:
 with zipfile.ZipFile(p/(zid+'.zip')) as z:
  lock=json.loads(z.read('userland/build/alpine-desktop-'+arch+'.lock.json'))
  current=json.loads((repo/('userland/alpine/locks/'+arch+'-desktop.json')).read_text())
  if isinstance(current,dict):current=current['packages']
  index=lambda es:{e['url']:e['sha256'] for e in es}
  print(arch,'artifact profile='+lock['profile'],'arch='+lock['arch'],'packages='+str(len(lock['packages'])), 'matches current pinned package map='+str(index(lock['packages'])==index(current)))
  assert index(lock['packages'])==index(current)
  for mode in ('desktop','kms'):
   t=z.read('build/test-alpine-'+mode+'-'+arch+'.log').decode(errors='replace').replace('\r','')
   pattern=r'su user -s /bin/sh -c alpine-udisks-smoke; echo (__RC\d+)=\$\?\n([^\n]*)\n\1=(\d+)\n'
   matches=list(re.finditer(pattern,t));assert len(matches)==1
   assert matches[0][2].endswith('uid=1000') and matches[0][3]=='0'
   print(arch,mode,'raw service command+uid1000+rc0 PASS')
  t=z.read('build/test-alpine-kms-'+arch+'.log').decode(errors='replace').replace('\r','')
  histories=re.findall(r'KONSOLE_QUERY_ATTEMPT_BEGIN slot=([^\n]+)\n(.*?)\nKONSOLE_QUERY_ATTEMPT_END slot=\1',t,re.S)
  assert len(histories)==6
  assert not any(re.search(rb'\bFAIL',body.encode()) for _,body in histories)
  for slot,body in histories:
   if slot in ('first','last'):
    print(arch,slot,body.splitlines()[0])
    print(' final lines=',[line for line in body.splitlines() if line.startswith(('terminal-dbus:','DBUS_A11Y_WITNESS'))][-2:])
# Execute the production shell recorder/loop in disposable mocked subprocesses.
path=repo/'scripts/terminal-attempt-history-host-tests.py'
n={'__file__':str(path),'__name__':'services_runtime_review'}
exec(compile(path.read_text(),str(path),'exec'),n)
readyterminal=n['TERMINAL'].replace('focused=0','focused=1')
readywitness=n['WITNESS'].replace('focused_states=0','focused_states=1').replace('qualified=0','qualified=1').replace('protocol_error=1','protocol_error=0')
readyfinal=n['FINAL'].replace('models=0','models=1').replace('NOT_OBSERVED','OBSERVED')
ready=readyterminal+readywitness+readyfinal
cases=[('live-dbus NOT_OBSERVED then OBSERVED','live-dbus',n['TERMINAL']+n['WITNESS']+n['FINAL'],ready),
 ('live-dbus remote-call then OBSERVED','live-dbus',n['REMOTE']+n['WITNESS']+n['FINAL'],ready),
 ('atspi NOT_READY then PASSED','atspi','A11Y_TERMINAL focused=0 showing=1 visible=1\ndesktopa11y: pid=777 application=1 models=0 visited=12 NOT_READY\n','A11Y_TERMINAL focused=1 showing=1 visible=1\ndesktopa11y: pid=777 application=1 models=1 visited=12 PASSED\n')]
for label,backend,first,last in cases:
 r,slots,calls=n['execute']([(1,first),(0,last)],backend=backend)
 match=bool(re.search(rb'\bFAIL',r.stdout.encode()))
 assert r.returncode==0 and len(calls)==2 and not match
 print('contract shell mock:',label,'original_rc=0 sentinel_hit='+str(match)+' host_would_pass=True')
# Do not overclaim arbitrary public-reference/error-text safety. This is a
# source-accepted public object path, not a path observed in these Qt guest logs.
line='DBUS_A11Y_QUERY method=GetState phase=begin bus=:1.7 path=/org/a11y/atspi/accessible/FAIL mono_ns=1\n'
r,slots,calls=n['execute']([(1,line+n['WITNESS']+n['FINAL']),(0,ready)])
assert r.returncode==0 and re.search(rb'\bFAIL',r.stdout.encode())
print('UNOBSERVED extended-path control: legal path suffix /FAIL; shell_rc=0 sentinel_hit=True host_would_pass=False')
print('No parser/source modifications and no guest execution.')
PY
git -C /data/9os-team-integration diff 18ef21fc34945f482d262993aa33690070aa86d5..07fda66 -- scripts/qemu-test.py
git -C /data/9os-team-integration status --short