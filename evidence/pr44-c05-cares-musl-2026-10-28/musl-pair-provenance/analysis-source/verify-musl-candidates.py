from pathlib import Path
import json,tarfile,subprocess,hashlib
r=Path('/data/cares-provenance');o=Path('/data/cares-maintenance/musl-provenance');v=r/'verifier';base=[str(v/'lib/ld-musl-x86_64.so.1'),'--library-path',f'{v}/lib:{v}/usr/lib',str(v/'sbin/apk'),'--keys-dir',str(v/'etc/apk/keys'),'--no-network'];candidates=[r/(n+'-1.2.5-r13.apk')for n in ['musl','musl-dev']];assert sum(p.stat().st_size for p in candidates)<=8*1024*1024
for n,args in [('pair-verify',['verify']+list(map(str,candidates))),('pair-native-index',['index','--output',str(o/'pair-generated-index.tar.gz')]+list(map(str,candidates)))]:
 cmd=base+args;x=subprocess.run(cmd,capture_output=True);(o/(n+'.stdout')).write_bytes(x.stdout);(o/(n+'.stderr')).write_bytes(x.stderr);(o/(n+'.command.json')).write_text(json.dumps({'argv':cmd,'rc':x.returncode},indent=2)+'\n');assert x.returncode==0;print(n,'rc',x.returncode,x.stdout.decode(),x.stderr.decode())
records=json.loads((o/'signed-index-musl-pair-and-exact-companions.json').read_text())['pair'];report=[]
with tarfile.open(o/'pair-generated-index.tar.gz','r:gz')as t:raw=t.extractfile('APKINDEX').read().decode()
(o/'pair-generated-index-records.txt').write_text(raw);generated=[dict(l.split(':',1)for l in b.splitlines()if ':'in l)for b in raw.strip().split('\n\n')]
for p in candidates:
 with tarfile.open(p,'r:gz')as t:
  members=t.getmembers();pkg=t.extractfile('.PKGINFO').read();(o/(p.stem+'.PKGINFO')).write_bytes(pkg);fields={}
  for l in pkg.decode().splitlines():
   if ' = 'in l:k,val=l.split(' = ',1);fields.setdefault(k,[]).append(val)
  name=fields['pkgname'][0];signed=records[name][0];g=[x for x in generated if x.get('P')==name];assert len(g)==1
  for k in ['C','P','V','A','S','I','D','p']:assert signed.get(k)==g[0].get(k),(name,k,signed.get(k),g[0].get(k))
  assert fields['pkgver']==['1.2.5-r13']and fields['arch']==['x86_64'];elfs=[]
  for m in members:
   if not m.isfile():continue
   f=t.extractfile(m);head=f.read(4)
   if head!=b'\x7fELF':continue
   data=head+f.read();q=o/'inspection'/p.stem/m.name;q.parent.mkdir(parents=True,exist_ok=True);q.write_bytes(data);x=subprocess.run(['readelf','-h','-d',str(q)],capture_output=True);assert x.returncode==0;text=x.stdout.decode();(o/(p.stem+'.'+m.name.replace('/','_')+'.readelf.txt')).write_text(text);elfs.append({'path':m.name,'size':len(data),'sha256':hashlib.sha256(data).hexdigest(),'headers':[l.strip()for l in text.splitlines()if any(v in l for v in ['Class:','Type:','Machine:','SONAME','NEEDED'])]})
  report.append({'filename':p.name,'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'size':p.stat().st_size,'fields':fields,'ELF':elfs,'members':[{'name':m.name,'size':m.size,'mode':oct(m.mode),'type':m.type.decode(),'linkname':m.linkname}for m in members],'signed_index_all_identity_checksum_fields_match':True})
 (o/(p.name+'.http.json')).write_bytes((r/(p.name+'.http.json')).read_bytes())
(o/'candidate-pair-report.json').write_text(json.dumps(report,indent=2)+'\n')
for x in report:print('CANDIDATE',x['filename'],x['size'],x['sha256'],'PKGINFO',x['fields'],'ELF',x['ELF'])
