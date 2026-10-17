#!/usr/bin/env python3
"""One fixed sample, fail closed, immutable evidence; no retry or fallback."""
from pathlib import Path
import hashlib,json,os,re,subprocess,sys,time
TOP=Path(__file__).resolve().parents[1];D=TOP/'build/m40-cold'
def evaluate(sample,text,rc,raw,integrity):
 totals=re.findall(r'^\d+/\d+ passed$',text,re.M);failures=re.findall(r'^FAIL   .*$',text,re.M)
 expected=['17/18 passed']if sample=='linux1'else['18/18 passed']
 allowed=['FAIL   grep -q "^violations 0" /proc/lockdep']if sample=='linux1'else[]
 return bool(integrity and raw==0 and rc==(1 if sample=='linux1'else 0)and totals==expected and failures==allowed and len(re.findall(r'^trace-snapshot-selftest: PASSED 3/3 synthetic process controls; not GUI proof$',text,re.M))==2 and re.search(r'^KONSOLE_FOCUSED_TERMINAL_READY$',text,re.M) and re.search(r'^KONSOLE_READINESS_BACKEND=live-dbus$',text,re.M) and 'QT_KEY_TRACE_LOADED'not in text and 'WAYLAND_KEY_TRACE 'not in text)
def active():
 for p in Path('/proc').iterdir():
  if not p.name.isdigit():continue
  try:args=(p/'cmdline').read_bytes().split(b'\0')
  except OSError:continue
  if any(a.rsplit(b'/',1)[-1].startswith(b'qemu-system-')and b' 'not in a and b'\n'not in a for a in args):return True
 return False
def main():
 assert len(sys.argv)==2 and sys.argv[1]in ['linux1','guest1'];sample=sys.argv[1]
 result=D/(sample+'-result.json');assert not result.exists(),'no retries'
 manifest=json.loads((D/'fixtures.json').read_text())
 def integrity():return not active()and all(hashlib.file_digest((TOP/p).open('rb'),'sha256').hexdigest()==h for p,h in manifest.items())
 if sample=='guest1':assert json.loads((D/'linux1-result.json').read_text())['accepted'],'control must pass first'
 info={'sample':sample,'started':time.time(),'accepted':False,'fixture_before':False,'fixture_after':False}
 result.write_text(json.dumps(info,indent=2)+'\n')
 info['fixture_before']=integrity()
 if not info['fixture_before']:raise SystemExit('STOP initial fixture mismatch or active VM')
 env={**os.environ,'COLD_SAMPLE':sample,'PATH':str(D/'adapters'/sample)+':'+os.environ['PATH']}
 with (D/(sample+'.out')).open('w')as log:
  rc=subprocess.run(['scripts/m40-cold-driver.sh','x86_64'],cwd=TOP,env=env,stdout=log,stderr=subprocess.STDOUT).returncode
 info['driver_rc']=rc;info['fixture_after']=integrity()
 with (D/(sample+'-raw-check.out')).open('w')as log:
  raw=subprocess.run([sys.executable,'scripts/input-capture-check.py',str(D/(sample+'.log'))],cwd=TOP,stdout=log,stderr=subprocess.STDOUT).returncode
 text=(D/(sample+'.out')).read_text(errors='replace')
 info.update(raw_rc=raw,totals=re.findall(r'^\d+/\d+ passed$',text,re.M),failures=re.findall(r'^FAIL   .*$',text,re.M),finished=time.time())
 info['accepted']=evaluate(sample,text,rc,raw,info['fixture_before']and info['fixture_after'])
 result.write_text(json.dumps(info,indent=2)+'\n');print(json.dumps(info,indent=2))
 if not info['accepted']:raise SystemExit(1)
if __name__=='__main__':main()
