import pathlib,subprocess,json,tarfile
p=pathlib.Path(__file__).parent; b=p/'bootstrap'
apk=[str(b/'lib/ld-musl-x86_64.so.1'),'--library-path',f'{b}/lib:{b}/usr/lib',str(b/'sbin/apk')]
results=[]
for keys,expected in [(b/'etc/apk/keys',0),(p/'empty-keys',1)]:
 name='trusted' if expected==0 else 'emptykeys'
 cmd=apk+['--keys-dir',str(keys),'verify',str(p/'linux-virt-6.12.114-r0.apk')]
 r=subprocess.run(cmd,capture_output=True)
 (p/(name+'.stdout')).write_bytes(r.stdout);(p/(name+'.stderr')).write_bytes(r.stderr)
 results.append(dict(name=name,argv=cmd,returncode=r.returncode))
 assert (r.returncode==0)==(expected==0),results
with tarfile.open(p/'APKINDEX.tar.gz') as t:
 index=t.extractfile('APKINDEX').read(); (p/'APKINDEX').write_bytes(index)
with tarfile.open(p/'linux-virt-6.12.114-r0.apk') as t:
 # Python tarfile stops after first concatenated tar stream; APK payload inspected separately.
 print('first stream members',len(t.getmembers()))
(p/'verification.json').write_text(json.dumps(results,indent=2)+'\n')
print([(x['name'],x['returncode']) for x in results])
