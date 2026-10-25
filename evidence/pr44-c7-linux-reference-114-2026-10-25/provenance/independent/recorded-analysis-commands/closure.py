import pathlib,json,urllib.request,hashlib,concurrent.futures,shutil,subprocess,tarfile,datetime
p=pathlib.Path(__file__).parent;repo=p/'local-repository/x86_64';b=p/'bootstrap'
blocks=[dict(line.split(':',1) for line in block.splitlines() if ':' in line) for block in (p/'APKINDEX').read_text().split('\n\n')]
idx={x['P']+'-'+x['V']+'.apk':x for x in blocks if 'P'in x}
names=[pathlib.Path(s).name for s in (p/'trusted-resolve-v2.stdout').read_text().splitlines()];assert len(names)==len(set(names))==25
shutil.copyfile(p/'linux-virt-6.12.114-r0.apk',repo/'linux-virt-6.12.114-r0.apk')
def get(name):
 x=idx[name];dst=repo/name;url='https://dl-cdn.alpinelinux.org/alpine/v3.22/main/x86_64/'+name
 if dst.exists():data=dst.read_bytes()
 else:
  with urllib.request.urlopen(url,timeout=60) as r:
   data=r.read();(p/(name+'.headers')).write_text(str(r.headers))
  dst.write_bytes(data)
 assert len(data)==int(x['S']),name
 with tarfile.open(dst) as t:info=t.extractfile('.PKGINFO').read()
 (p/(name+'.PKGINFO')).write_bytes(info)
 record=dict(url=url,bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),index=x)
 (p/(name+'.metadata.json')).write_text(json.dumps(record,indent=2)+'\n')
 return record
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as e:records=list(e.map(get,names))
(p/'closure-records.json').write_text(json.dumps(records,indent=2)+'\n')
apk=[str(b/'lib/ld-musl-x86_64.so.1'),'--library-path',f'{b}/lib:{b}/usr/lib',str(b/'sbin/apk')]
cmd=apk+['--keys-dir',str(b/'etc/apk/keys'),'verify']+[str(repo/n) for n in names]
r=subprocess.run(cmd,capture_output=True);(p/'all25-verify.stdout').write_bytes(r.stdout);(p/'all25-verify.stderr').write_bytes(r.stderr);(p/'all25-verify.command.json').write_text(json.dumps(dict(argv=cmd,returncode=r.returncode),indent=2)+'\n');assert r.returncode==0
print('25 files, total',sum(x['bytes']for x in records),'bytes; native verify RC',r.returncode)
