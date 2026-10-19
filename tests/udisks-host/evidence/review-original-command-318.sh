PYTHONDONTWRITEBYTECODE=1 python3 - <<'PY'
from pathlib import Path
import json,zipfile,re
p=Path('/data/pr42-current');repo=Path('/data/9os-team-integration')
for arch,zid in [('aarch64','12151762243'),('x86_64','12151532724'),('riscv64','12151722599')]:
 with zipfile.ZipFile(p/(zid+'.zip')) as z:
  lock=json.loads(z.read('userland/build/alpine-desktop-'+arch+'.lock.json'))
  assert lock['profile']=='desktop' and lock['arch']==arch
  assert all(re.fullmatch('[0-9a-f]{64}',e['sha256']) for e in lock['packages'])
  print(arch,'arch-specific preserved package hashes='+str(len(lock['packages'])))
  if arch=='x86_64':
   current=json.loads((repo/'userland/alpine/locks/x86_64-desktop.json').read_text())
   ix=lambda es:{e['url']:e['sha256'] for e in es}
   assert ix(lock['packages'])==ix(current['packages'])
   print(' x86 checked-in pinned package map equals artifact map')
  t=z.read('build/test-alpine-kms-'+arch+'.log').decode(errors='replace').replace('\r','')
  histories=re.findall(r'KONSOLE_QUERY_ATTEMPT_BEGIN slot=([^\n]+)\n(.*?)\nKONSOLE_QUERY_ATTEMPT_END slot=\1',t,re.S)
  assert len(histories)==6
  assert not any(re.search(rb'\bFAIL',body.encode()) for _,body in histories)
  for slot,body in histories:
   if slot in ('first','last'):
    print(' ',slot,body.splitlines()[0])
    print(' ',[line for line in body.splitlines() if line.startswith('terminal-dbus:')][-1])
path=repo/'scripts/terminal-attempt-history-host-tests.py'
n={'__file__':str(path),'__name__':'services_runtime_review'}
exec(compile(path.read_text(),str(path),'exec'),n)
ready=n['TERMINAL'].replace('focused=0','focused=1')+n['WITNESS'].replace('focused_states=0','focused_states=1').replace('qualified=0','qualified=1').replace('protocol_error=1','protocol_error=0')+n['FINAL'].replace('models=0','models=1').replace('NOT_OBSERVED','OBSERVED')
cases=[('live-dbus NOT_OBSERVED then OBSERVED','live-dbus',n['TERMINAL']+n['WITNESS']+n['FINAL'],ready),
 ('live-dbus remote-call then OBSERVED','live-dbus',n['REMOTE']+n['WITNESS']+n['FINAL'],ready),
 ('atspi NOT_READY then PASSED','atspi','A11Y_TERMINAL focused=0 showing=1 visible=1\ndesktopa11y: pid=777 application=1 models=0 visited=12 NOT_READY\n','A11Y_TERMINAL focused=1 showing=1 visible=1\ndesktopa11y: pid=777 application=1 models=1 visited=12 PASSED\n')]
for label,backend,first,last in cases:
 r,slots,calls=n['execute']([(1,first),(0,last)],backend=backend)
 match=bool(re.search(rb'\bFAIL',r.stdout.encode()))
 assert r.returncode==0 and len(calls)==2 and not match
 print('contract shell mock:',label,'shell_rc=0 sentinel_hit=False host_would_pass=True')
line='DBUS_A11Y_QUERY method=GetState phase=begin bus=:1.7 path=/org/a11y/atspi/accessible/FAIL mono_ns=1\n'
r,slots,calls=n['execute']([(1,line+n['WITNESS']+n['FINAL']),(0,ready)])
assert r.returncode==0 and re.search(rb'\bFAIL',r.stdout.encode())
print('UNOBSERVED extended-path control: source-accepted path suffix /FAIL; shell_rc=0 sentinel_hit=True host_would_pass=False')
print('No parser/source changes or guest execution.')
PY