from pathlib import Path
import subprocess,json,os,hashlib,datetime,time
r=Path('/data/cares-maintenance');p=r/'product';ev=r/'evidence';tools=r/'fresh-tools-once';target=p/'userland/alpine-root-x86_64'
assert not tools.exists() and not target.exists();tools.mkdir();assert not list(tools.iterdir())
status=Path('/proc/self/status').read_text();(ev/'native-parent-proc-status.txt').write_text(status)
meta={'argv':['timeout','-k','10','600','python3','userland/alpine/build.py','--arch','x86_64','--profile','base','--lock','userland/alpine/locks/x86_64.json'],'cwd':str(p),'environment_overrides':{'TOOLS_DIR':str(tools),'PYTHONDONTWRITEBYTECODE':'1'},'uid':os.getuid(),'euid':os.geteuid(),'gid':os.getgid(),'groups':os.getgroups(),'empty_tools_before':True,'target_absent_before':True,'builder_sha256':hashlib.sha256((p/'userland/alpine/build.py').read_bytes()).hexdigest(),'base_lock_sha256':hashlib.sha256((p/'userland/alpine/locks/x86_64.json').read_bytes()).hexdigest(),'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'attempts':1,'watchdog_seconds':600,'kill_grace_seconds':10}
(ev/'native-command-before.json').write_text(json.dumps(meta,indent=2)+'\n')
t=time.monotonic()
with (ev/'native.stdout').open('wb')as out,(ev/'native.stderr').open('wb')as err:
 x=subprocess.run(meta['argv'],cwd=p,env=dict(os.environ,**meta['environment_overrides']),stdout=out,stderr=err)
meta.update(rc=x.returncode,elapsed_seconds=time.monotonic()-t,finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),target_exists_after=target.exists(),cache_entries_after=len(list(tools.rglob('*'))));(ev/'native-result.json').write_text(json.dumps(meta,indent=2)+'\n');print(json.dumps(meta,indent=2));print('STDERR_TAIL\n'+(ev/'native.stderr').read_text(errors='replace')[-2400:]);print('STDOUT_TAIL\n'+(ev/'native.stdout').read_text(errors='replace')[-700:]);raise SystemExit(x.returncode)
