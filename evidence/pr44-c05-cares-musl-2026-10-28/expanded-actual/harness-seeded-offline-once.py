from pathlib import Path
import subprocess,os,json,hashlib,shutil,datetime,time
r=Path('/data/cares-maintenance');p=r/'seeded-product-once';tools=r/'seeded-tools-once';ev=r/'expanded-evidence';old=r/'product';target=p/'userland/alpine-root-x86_64';failed=old/'userland/alpine-root-x86_64'
assert not p.exists() and not tools.exists() and not failed.exists()
# Clone only source files, not bytecode or earlier build products.
shutil.copytree(old,p,ignore=shutil.ignore_patterns('__pycache__','build','alpine-root-*'))
cache=tools/'dl/alpine/x86_64';cache.mkdir(parents=True)
lock=json.loads((p/'userland/alpine/locks/x86_64.json').read_text());seed=[]
for ent in lock['packages']:
 name=ent['url'].rsplit('/',1)[-1];src=Path('/data/cares-provenance')/name if name in ['musl-1.2.5-r13.apk','musl-dev-1.2.5-r13.apk'] else r/'fresh-tools-once/dl/alpine/x86_64'/name
 b=src.read_bytes();sha=hashlib.sha256(b).hexdigest();assert sha==ent['sha256'];shutil.copyfile(src,cache/name);seed.append(dict(ent,size=len(b),source=str(src),actual_sha256=sha))
mini='alpine-minirootfs-3.22.6-x86_64.tar.gz';src=r/'fresh-tools-once/dl/alpine/x86_64'/mini;b=src.read_bytes();assert hashlib.sha256(b).hexdigest()=='27694aaa55fd7a9e3ef596e0ad4eb66802308bb20172b17030cd5f4d8ae9bac2';shutil.copyfile(src,cache/mini)
(ev/'seed-manifest.json').write_text(json.dumps({'packages':seed,'mini':{'name':mini,'sha256':hashlib.sha256(b).hexdigest(),'size':len(b)},'counts':{'retained':158,'new_verified_pair':2}},indent=2)+'\n')
(ev/'offline-parent-proc-status.txt').write_text(Path('/proc/self/status').read_text())
meta={'argv':['timeout','-k','10','600','python3','userland/alpine/build.py','--arch','x86_64','--profile','base','--lock','userland/alpine/locks/x86_64.json','--offline'],'cwd':str(p),'environment_overrides':{'TOOLS_DIR':str(tools),'PYTHONDONTWRITEBYTECODE':'1'},'uid':os.getuid(),'euid':os.geteuid(),'gid':os.getgid(),'groups':os.getgroups(),'new_tools_absent_before_seed':True,'new_target_absent':not target.exists(),'original_failed_target_still_absent':not failed.exists(),'builder_sha256':hashlib.sha256((p/'userland/alpine/build.py').read_bytes()).hexdigest(),'lock_sha256':hashlib.sha256((p/'userland/alpine/locks/x86_64.json').read_bytes()).hexdigest(),'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'classification':'ONE SEEDED OFFLINE coherence/security experiment, NOT fresh-acquisition/cold CI proof'}
assert meta['new_target_absent'] and meta['original_failed_target_still_absent'];(ev/'offline-command-before.json').write_text(json.dumps(meta,indent=2)+'\n')
t=time.monotonic()
with (ev/'offline.stdout').open('wb') as out,(ev/'offline.stderr').open('wb') as err:
 x=subprocess.run(meta['argv'],cwd=p,env=dict(os.environ,**meta['environment_overrides']),stdout=out,stderr=err)
meta.update(rc=x.returncode,elapsed_seconds=time.monotonic()-t,finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),target_exists_after=target.exists())
(ev/'offline-result.json').write_text(json.dumps(meta,indent=2)+'\n');print(json.dumps(meta,indent=2));print('STDERR', (ev/'offline.stderr').read_text());print('STDOUTTAIL', (ev/'offline.stdout').read_text()[-1200:]);raise SystemExit(x.returncode)
