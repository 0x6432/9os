import pathlib,subprocess,json,shutil,hashlib
p=pathlib.Path(__file__).parent;b=p/'bootstrap';repo=p/'local-repository';repo.mkdir();shutil.copyfile(p/'APKINDEX.tar.gz',repo/'APKINDEX.tar.gz')
apk=[str(b/'lib/ld-musl-x86_64.so.1'),'--library-path',f'{b}/lib:{b}/usr/lib',str(b/'sbin/apk')]
records=[]
for label,keys in [('trusted',b/'etc/apk/keys'),('emptykeys',p/'empty-keys')]:
 root=p/('resolve-'+label);(root/'etc/apk').mkdir(parents=True);(root/'etc/apk/world').write_text('linux-virt=6.12.114-r0\n');(root/'etc/apk/repositories').write_text(str(repo)+'\n')
 cmd=apk+['--arch','x86_64','--root',str(root),'--keys-dir',str(keys),'--repositories-file',str(root/'etc/apk/repositories'),'--no-network','fetch','--world','--recursive','--simulate','--url']
 r=subprocess.run(cmd,capture_output=True,cwd=root);(p/(label+'-resolve.stdout')).write_bytes(r.stdout);(p/(label+'-resolve.stderr')).write_bytes(r.stderr)
 records.append(dict(label=label,argv=cmd,returncode=r.returncode))
 if label=='trusted':assert r.returncode==0,(r.returncode,r.stderr)
 else:assert r.returncode!=0,(r.returncode,r.stderr)
(p/'resolve-commands.json').write_text(json.dumps(records,indent=2)+'\n')
print([(x['label'],x['returncode']) for x in records]);print((p/'trusted-resolve.stdout').read_text())
