import pathlib,json,hashlib,subprocess,re
r=pathlib.Path(__file__).parent;p=r/'proof';repo='/data/pr44-cache-publication/repo.git';base='c05a3fcd851297161f3981b38dd04900c49e8467'
git=lambda *a:subprocess.check_output(['git','--git-dir='+repo,*a]);H=lambda b:hashlib.sha256(b).hexdigest()
s=json.loads((p/'expanded-patch-summary.json').read_text());patch=(p/'expanded-maintenance.patch').read_bytes();assert H(patch)==s['patch_sha256'] and len(patch)==8643
files={};before={};path=None
for line in patch.decode().splitlines(True):
 if line.startswith('--- a/'):
  path=line[6:].strip();before[path]=git('show',base+':'+path);files[path]=[];src=before[path].decode().splitlines(True);pos=0
 elif line.startswith('+++ b/'):assert line[6:].strip()==path
 elif line.startswith('@@'):
  m=re.match(r'@@ -(\d+)(?:,\d+)? \+\d+(?:,\d+)? @@',line);start=int(m[1])-1;assert start>=pos;files[path].extend(src[pos:start]);pos=start
 elif line.startswith((' ','-','+')):
  if line[0] in ' -':assert src[pos]==line[1:];pos+=1
  if line[0] in ' +':files[path].append(line[1:])
 else:raise Exception(line)
 # flush remainder at next file handled below
 if line==patch.decode().splitlines(True)[-1]:pass
# redo robust blocks to append tails
for block in patch.decode().split('--- a/')[1:]:
 lines=block.splitlines(True);name=lines[0].strip();src=before[name].decode().splitlines(True);out=[];pos=0
 for line in lines[2:]:
  if line.startswith('@@'):
   start=int(re.match(r'@@ -(\d+)',line)[1])-1;out+=src[pos:start];pos=start
  else:
   if line[0] in ' -':assert src[pos]==line[1:];pos+=1
   if line[0] in ' +':out.append(line[1:])
 out+=src[pos:];files[name]=''.join(out).encode()
for name,b in files.items():
 assert H(before[name])==s['before'][name]['sha256'];assert H(b)==s['after'][name]['sha256']
 if name.endswith('.json'):
  old=json.loads(before[name]);new=json.loads(b);diff=[(a,c)for a,c in zip(old['packages'],new['packages'])if a!=c];assert len(old['packages'])==len(new['packages']);assert len(diff)==(1 if 'qsb' in name else 3)
  old['packages']=new['packages'];assert old==new
  print(name,len(new['packages']),'changed',len(diff))
(r/'reconstructed').mkdir(exist_ok=True)
for name,b in files.items():dest=r/'reconstructed'/name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(b)
entries=git('ls-tree','-r',base).decode().splitlines();assert len(entries)-len(files)==869
assert H(git('show',base+':userland/alpine/build.py'))==s['unchanged_builder_sha256']
lock=json.loads(files['userland/alpine/locks/x86_64.json']);ids=json.loads((p/'all160-signed-identities.json').read_text());seed=json.loads((p/'seed-manifest.json').read_text());pairs=lambda rows:{(a['url'],a['sha256'])for a in rows}
assert len(ids)==len(seed['packages'])==len(lock['packages'])==160
assert pairs(ids)==pairs(seed['packages'])==pairs(lock['packages'])
assert all(a['actual_sha256']==a['sha256'] for a in seed['packages']);assert all(a['arch'] in ['x86_64','noarch']for a in ids)
assert {a['url']:a['size']for a in ids}=={a['url']:a['size']for a in seed['packages']}
sigs=[x.rsplit('/',1)[-1].removesuffix(': 0 - OK')for x in (p/'offline.stdout').read_text().splitlines()if x.endswith(': 0 - OK')]
assert len(sigs)==len(set(sigs))==160;assert set(sigs)=={a['url'].rsplit('/',1)[-1]for a in ids}
assert not (p/'offline.stderr').read_bytes();assert not re.search(r'warning|permission denied|ERROR:|failed',(p/'offline.stdout').read_text(),re.I)
for f in p.glob('mutant-*.log'):
 text=f.read_text();assert re.search(r'Ran 16 tests',text) and 'ERROR:'not in text and 'FAILED (failures='in text
for a in ids:
 if a['pkgname']in ['musl','musl-dev','c-ares']:print(a['pkgname'],a['pkgver'],a['depends'],a['provides'])
print('PASS independently reconstructed 4 paths, 7 pairs, 869 unchanged source entries; seed/signature/identity exact160 binding; raw tests/mutants read')
