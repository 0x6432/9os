import urllib.request,hashlib,pathlib,json,datetime
p=pathlib.Path(__file__).parent
base='https://dl-cdn.alpinelinux.org/alpine/v3.22/'
items=[('mini.tar.gz',base+'releases/x86_64/alpine-minirootfs-3.22.6-x86_64.tar.gz',3646244,'27694aaa55fd7a9e3ef596e0ad4eb66802308bb20172b17030cd5f4d8ae9bac2'),('APKINDEX.tar.gz',base+'main/x86_64/APKINDEX.tar.gz',500486,'0bb20ca8f6082e981229755622164e0ce8de535d57558c44b034868d126f00ef'),('linux-virt-6.12.114-r0.apk',base+'main/x86_64/linux-virt-6.12.114-r0.apk',40732552,'82b7fee968e553fb34df7102e9622c969872810b729068106ed49a03c0ba4b8a')]
for name,url,size,sha in items:
 dst=p/name
 if dst.exists(): raise SystemExit('Refusing overwrite '+name)
 start=datetime.datetime.now(datetime.timezone.utc).isoformat()
 with urllib.request.urlopen(url,timeout=60) as r:
  headers=str(r.headers); data=r.read(); status=r.status
 dst.write_bytes(data)
 (p/(name+'.headers')).write_text(headers)
 actual=hashlib.sha256(data).hexdigest()
 rec=dict(url=url,start=start,status=status,bytes=len(data),sha256=actual,expected_sha256=sha)
 (p/(name+'.acquisition.json')).write_text(json.dumps(rec,indent=2)+'\n')
 print(name,len(data),actual,flush=True)
 if len(data)!=size or actual!=sha: raise SystemExit('Pinned byte mismatch; stopped')
