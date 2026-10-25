import pathlib,json,hashlib,tarfile,re,os,stat
r=pathlib.Path('/data/ca-maintenance-product');e=pathlib.Path(__file__).parent;root=r/'userland/alpine-root-x86_64';cache=pathlib.Path('/data/ca-maintenance-clean-tools-once/dl/alpine/x86_64')
assert json.loads((e/'native-status.json').read_text())['returncode']==0
lock=json.loads((r/'userland/alpine/locks/x86_64.json').read_text()); manifest=json.loads((root/'etc/9os-alpine.json').read_text());assert sorted(lock['packages'],key=lambda p:p['url'])==manifest['packages']
metadata=[]
for x in lock['packages']:
 f=cache/x['url'].rsplit('/',1)[-1];assert hashlib.sha256(f.read_bytes()).hexdigest()==x['sha256']
 with tarfile.open(f,'r:gz',ignore_zeros=True)as t:
  raw=t.extractfile('.PKGINFO').read().decode();d={}
  for l in raw.splitlines():
   if ' = ' in l:
    k,v=l.split(' = ',1);d.setdefault(k,[]).append(v)
 metadata.append(dict(x,size=f.stat().st_size,pkginfo=d))
(e/'native-all160-verified-metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
stdout=(e/'native.stdout').read_text();oks=re.findall(r'^.*\.apk: 0 - OK$',stdout,re.M);assert len(oks)==160,len(oks)
db=(root/'lib/apk/db/installed').read_text();(e/'native-installed-db').write_text(db);installed={}
for block in db.split('\n\n'):
 d={}
 for l in block.splitlines():
  if len(l)>2 and l[1]==':':d[l[0]]=l[2:]
 if 'P'in d:installed[d['P']]=d
for x in metadata:
 d=x['pkginfo'];assert installed[d['pkgname'][0]]['V']==d['pkgver'][0]
assert installed['ca-certificates-bundle']['V']=='20261020-r0'
ca=(root/'etc/ssl/certs/ca-certificates.crt').read_bytes();assert hashlib.sha256(ca).hexdigest()=='5961c0a3c65459931c023e0075c09733f09aea4ca439cb85bc4a08bb4f3f568b'
assert stat.S_IMODE((root/'bin/busybox').stat().st_mode)==0o4755
cap=os.getxattr(root/'usr/bin/sway','security.capability');assert cap
(root/'etc/9os-alpine.json').read_bytes();(e/'native-root-manifest.json').write_bytes((root/'etc/9os-alpine.json').read_bytes())
security={'sway_security_capability_hex':cap.hex(),'busybox_mode':oct(stat.S_IMODE((root/'bin/busybox').stat().st_mode)),'ca_payload_sha256':hashlib.sha256(ca).hexdigest(),'installed_package_count':len(installed),'locked_count':160,'apk_signature_OK_lines':len(oks),'installed_ca':installed['ca-certificates-bundle'],'old_ca_dependencies_in_verified_input':[{'name':x['pkginfo']['pkgname'],'depends':x['pkginfo'].get('depend',[])}for x in metadata if any('20260909' in d and 'ca-certificates' in d for d in x['pkginfo'].get('depend',[]))]}
assert not security['old_ca_dependencies_in_verified_input'];(e/'native-installed-security-and-dependencies.json').write_text(json.dumps(security,indent=2)+'\n');print(json.dumps(security,indent=2))
# Full regular-file bytes, symlink targets and modes, plus capability xattr of the root.
records=[]
for p in sorted(root.rglob('*')):
 st=p.lstat(); rec={'path':str(p.relative_to(root)),'mode':oct(stat.S_IMODE(st.st_mode))}
 if p.is_symlink():rec['symlink']=os.readlink(p)
 elif p.is_file():rec['size']=st.st_size;rec['sha256']=hashlib.sha256(p.read_bytes()).hexdigest()
 elif p.is_dir():rec['type']='directory'
 else:rec['type']='special'
 records.append(rec)
(e/'native-root-files-manifest.json').write_text(json.dumps(records,indent=2)+'\n');print('root_manifest_entries',len(records))
