from pathlib import Path
import hashlib,importlib.util,json,shutil,time,traceback
product=Path('/data/linux-oracle-114-product');evidence=Path('/data/linux-oracle-114-evidence');analysis=Path('/data/linux-reference-114-analysis');run=Path('/data/linux-oracle-114-one-assembly')
if run.exists():raise RuntimeError('Refusing to repeat the one authorized real assembly')
run.mkdir();cache=run/'cache';target=run/'target';cache.mkdir();target.mkdir()
entries=json.loads((product/'userland/ports/linux-oracle-lock.json').read_text())['packages']
paths=[analysis/'verified-copy'/e['url'].rsplit('/',1)[-1] for e in entries]+[analysis/'originals/alpine-minirootfs-3.22.6-x86_64.tar.gz']
def inventory(paths):return [{'path':str(p),'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in paths]
before=inventory(paths);proof={'mode':'ONE REAL native OFFLINE assembly, not a VM/boot','target_before':list(target.iterdir()),'cache_before':list(cache.iterdir()),'original_source_S_root_supplied':False,'S_root_scope':'No S-root is passed to helper. Original oracle consumer/BusyBox/musl copy tail is byte-identical except explicit module ABI update, verified by native source test. No claim of an unavailable historical S-root byte census.','original_acquisition_inputs_before':before,'commands':[]}
for source in paths:shutil.copyfile(source,cache/source.name)
spec=importlib.util.spec_from_file_location('actual_locked_oracle',product/'scripts/evdev-linux-oracle-packages.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
def record(command):
 proof['commands'].append(command)
 if 'add' in command and '--simulate' not in command:
  assert not any(target.iterdir()),'Actual target not empty before add'
  proof['target_empty_immediately_before_actual_add']=True
 (evidence/'one-assembly-progress.json').write_text(json.dumps(proof,indent=2,default=str)+'\n')
 return m.checked(command)
start=time.monotonic()
try:
 result=m.assemble(product/'userland/ports/linux-oracle-lock.json',cache,target,offline=True,runner=record)
 (evidence/'one-assembly-installed-manifest.json').write_text(json.dumps(result,indent=2)+'\n');proof['success']=True
finally:
 proof['elapsed_s']=round(time.monotonic()-start,3);proof['original_acquisition_inputs_after']=inventory(paths);proof['original_inputs_byte_identical']=proof['original_acquisition_inputs_before']==proof['original_acquisition_inputs_after'];proof['target_file_count_after']=sum(p.is_file() for p in target.rglob('*'));proof['actual_add_count']=sum('add' in c and '--simulate' not in c for c in proof['commands']);(evidence/'one-assembly-proof.json').write_text(json.dumps(proof,indent=2,default=str)+'\n')
print('ONE REAL OFFLINE ASSEMBLY SUCCESS; seconds',proof['elapsed_s'],'actualaddcount',proof['actual_add_count'],'originalinputsunmodified',proof['original_inputs_byte_identical'])
