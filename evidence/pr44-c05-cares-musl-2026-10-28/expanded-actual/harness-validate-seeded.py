from pathlib import Path
import json,os,hashlib,tarfile,stat,subprocess,re
r=Path('/data/cares-maintenance');e=r/'expanded-evidence';p=r/'seeded-product-once';root=p/'userland/alpine-root-x86_64';cache=r/'seeded-tools-once/dl/alpine/x86_64'
sha=lambda b:hashlib.sha256(b).hexdigest()
def db(raw):
 rows=[]
 for block in raw.strip().split('\n\n'):
  v={}
  for line in block.splitlines():
   if len(line)>2 and line[1]==':':v.setdefault(line[0],[]).append(line[2:])
  if 'P'in v:rows.append(v)
 names=[a['P'][0]for a in rows];assert len(set(names))==len(names)
 return {a['P'][0]:a['V'][0]for a in rows}
mini=cache/'alpine-minirootfs-3.22.6-x86_64.tar.gz'
with tarfile.open(mini) as tf: bootstrap=tf.extractfile(next(m for m in tf.getmembers() if m.name.lstrip('./')=='lib/apk/db/installed')).read()
(e/'bootstrap-installed.txt').write_bytes(bootstrap);installed=(root/'lib/apk/db/installed').read_bytes();(e/'installed.txt').write_bytes(installed)
expected=db(bootstrap.decode());before=dict(expected)
lock=json.loads((p/'userland/alpine/locks/x86_64.json').read_text());identities=[];signed_paths=[]
for ent in lock['packages']:
 f=cache/ent['url'].rsplit('/',1)[-1];b=f.read_bytes();assert sha(b)==ent['sha256']
 with tarfile.open(f) as tf:
  raw=tf.extractfile('.PKGINFO').read().decode();fields={}
  for line in raw.splitlines():
   if ' = 'in line:k,v=line.split(' = ',1);fields.setdefault(k,[]).append(v)
 name=fields['pkgname'][0];ver=fields['pkgver'][0];expected[name]=ver
 identities.append(dict(ent,size=len(b),pkgname=name,pkgver=ver,arch=fields['arch'][0],depends=fields.get('depend',[]),provides=fields.get('provides',[])))
 signed_paths.append(str(f)+': 0 - OK')
actual=db(installed.decode());assert actual==expected,(set(actual.items())^set(expected.items()))
stdout=(e/'offline.stdout').read_text();sig=[a for a in stdout.splitlines()if a.endswith(': 0 - OK')];assert len(sig)==160 and set(sig)==set(signed_paths)
assert not(e/'offline.stderr').read_bytes();assert not re.search(r'(?i)warning|permission denied|ERROR:|failed',stdout)
(e/'all160-signed-identities.json').write_text(json.dumps(identities,indent=2)+'\n')
manifest=[]
for d,ds,fs in os.walk(root,followlinks=False):
 for n in sorted(ds+fs):
  f=Path(d)/n;st=f.lstat();v={'path':str(f.relative_to(root)),'mode':oct(stat.S_IMODE(st.st_mode)),'uid':st.st_uid,'gid':st.st_gid,'type':'symlink'if stat.S_ISLNK(st.st_mode)else'directory'if stat.S_ISDIR(st.st_mode)else'file','size':st.st_size}
  if v['type']=='file':v['sha256']=sha(f.read_bytes())
  if v['type']=='symlink':v['target']=os.readlink(f)
  try:v['xattrs']={k:os.getxattr(f,k,follow_symlinks=False).hex()for k in os.listxattr(f,follow_symlinks=False)}
  except OSError as er:raise RuntimeError('xattr inspection failed:'+str(f))from er
  manifest.append(v)
manifest.sort(key=lambda a:a['path']);(e/'file-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
security={}
for rel,mode in [('bin/busybox',0o4755),('etc/shadow',0o640),('usr/bin/sway',0o755)]:
 f=root/rel;st=f.stat();assert stat.S_IMODE(st.st_mode)==mode;security[rel]={'mode':oct(mode),'uid':st.st_uid,'gid':st.st_gid}
sway=next(a for a in lock['packages']if a['url'].rsplit('/',1)[-1].startswith('sway-1.'))
with tarfile.open(cache/sway['url'].rsplit('/',1)[-1]) as tf:
 hdr=tf.getmember('usr/bin/sway');expectedcap=hdr.pax_headers['SCHILY.xattr.security.capability'].encode('utf-8','surrogateescape')
actualcap=os.getxattr(root/'usr/bin/sway','security.capability');assert actualcap==expectedcap
security['usr/bin/sway'].update(capability_hex=actualcap.hex(),signed_archive_capability_hex=expectedcap.hex())
(e/'getcap.stdout').write_bytes(subprocess.run(['getcap',str(root/'usr/bin/sway')],capture_output=True,check=True).stdout)
abi={}
for rel in ['usr/lib/libcares.so.2','lib/ld-musl-x86_64.so.1']:
 f=root/rel;assert f.exists();out=subprocess.run(['readelf','-h','-d',str(f)],capture_output=True,check=True);(e/(rel.replace('/','_')+'.readelf.txt')).write_bytes(out.stdout)
 text=out.stdout.decode();expectedsoname='libcares.so.2'if'cares'in rel else'libc.musl-x86_64.so.1';assert '[%s]'%expectedsoname in text and 'X86-64'in text
 abi[rel]={'sha256':sha(f.read_bytes()),'size':f.stat().st_size,'realpath':str(f.resolve().relative_to(root)),'soname':expectedsoname}
summary={'base_lock_count':len(lock['packages']),'signature_OK_distinct_count':len(sig),'installed_exact_union':True,'installed_count':len(actual),'bootstrap_only_count':len(set(before)-{a['pkgname']for a in identities}),'installed_db_sha256':sha(installed),'bootstrap_db_sha256':sha(bootstrap),'file_manifest_count':len(manifest),'file_manifest_sha256':sha((e/'file-manifest.json').read_bytes()),'security':security,'abi':abi,'stderr_bytes':0,'warning_scan':'no warnings/errors matched','qualification':'UID1000 with broad capabilities/NNP0; NOT zero-cap CI or whole-root ownership proof. Seeded offline only; no cold acquisition, desktop/QSB integration, VM or guest boot.'}
(e/'installed-union-security-summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary,indent=2))
