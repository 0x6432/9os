from pathlib import Path
import json,hashlib,tarfile,subprocess,datetime
r=Path('/data/cares-maintenance');c=r/'fresh-tools-once/dl/alpine/x86_64';v=Path('/data/cares-provenance/verifier');out=r/'musl-provenance';assert not out.exists();out.mkdir()
lock=json.loads((r/'product/userland/alpine/locks/x86_64.json').read_text());paths=[];metadata=[]
for x in lock['packages']:
 p=c/x['url'].rsplit('/',1)[-1]
 if p.exists():
  assert hashlib.sha256(p.read_bytes()).hexdigest()==x['sha256'];paths.append(p)
assert len(paths)==159
cmd=[str(v/'lib/ld-musl-x86_64.so.1'),'--library-path',f'{v}/lib:{v}/usr/lib',str(v/'sbin/apk'),'--keys-dir',str(v/'etc/apk/keys'),'--no-network','verify']+list(map(str,paths));z=subprocess.run(cmd,capture_output=True);(out/'cached159-verify.stdout').write_bytes(z.stdout);(out/'cached159-verify.stderr').write_bytes(z.stderr);(out/'cached159-verify.command.json').write_text(json.dumps({'argv':cmd,'rc':z.returncode,'utc':datetime.datetime.now(datetime.timezone.utc).isoformat()},indent=2)+'\n');assert z.returncode==0
for p in paths:
 with tarfile.open(p,'r:gz')as t:data=t.extractfile('.PKGINFO').read().decode()
 d={}
 for l in data.splitlines():
  if ' = ' in l:
   k,val=l.split(' = ',1);d.setdefault(k,[]).append(val)
 metadata.append({'filename':p.name,'size':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'fields':d,'raw_PKGINFO':data})
(out/'cached159-signed-PKGINFO.json').write_text(json.dumps(metadata,indent=2)+'\n')
rev=[{'filename':x['filename'],'pkgname':x['fields'].get('pkgname'),'pkgver':x['fields'].get('pkgver'),'depend':x['fields'].get('depend',[]),'provides':x['fields'].get('provides',[])}for x in metadata if x['fields'].get('pkgname')==['musl']or any('musl'in d for d in x['fields'].get('depend',[]))]
(out/'cached159-musl-reverse-constraints.json').write_text(json.dumps(rev,indent=2)+'\n')
with tarfile.open('/data/cares-provenance/APKINDEX.tar.gz','r:gz')as t:raw=t.extractfile('APKINDEX').read().decode()
records=[]
for b in raw.split('\n\n'):
 d=dict(l.split(':',1)for l in b.splitlines()if ':'in l)
 if d:records.append(d)
pair={name:[d for d in records if d.get('P')==name]for name in ['musl','musl-dev']};assert all(len(x)==1 for x in pair.values());a=pair['musl'][0];b=pair['musl-dev'][0];assert a['V']==b['V']and a['A']==b['A']=='x86_64';assert 'musl='+a['V']in b['D'].split();assert sum(int(x[0]['S'])for x in pair.values())<=8*1024*1024
companions=[{k:d.get(k)for k in ['P','V','A','D','p']}for d in records if any(('musl='in token or'musl-dev='in token)for token in d.get('D','').split())]
(out/'signed-index-musl-pair-and-exact-companions.json').write_text(json.dumps({'index_sha256':'86cfe402b7968736024b67ca1d1334ae5824371822ae803e271af9675d280f3b','pair':pair,'all_exact_musl_or_musl_dev_constraints':companions,'cached_exact_constraints':[x for x in rev if any('musl='in d or'musl-dev='in d for d in x['depend'])]},indent=2)+'\n')
print('cached159signature_rc',z.returncode,'parsed',len(metadata),'cachedexactconstraints',[x for x in rev if any('musl='in d or'musl-dev='in d for d in x['depend'])]);print('CANDIDATEPAIR',json.dumps(pair,indent=2));print('INDEXEXACTCOMPANIONS',json.dumps(companions,indent=2))
