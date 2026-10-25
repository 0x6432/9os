import pathlib,subprocess,json,hashlib
p=pathlib.Path(__file__).parent;b=p/'bootstrap';files=sorted((p/'local-repository/x86_64').glob('*.apk'))
apk=[str(b/'lib/ld-musl-x86_64.so.1'),'--library-path',f'{b}/lib:{b}/usr/lib',str(b/'sbin/apk')]
records=[]
# Local fetch validates signed index package identities; only copies package archives.
out=p/'validated-local-apks';out.mkdir();root=p/'resolve-v2-trusted'
cmd=apk+['--root',str(root),'--arch','x86_64','--keys-dir',str(b/'etc/apk/keys'),'--repositories-file',str(root/'etc/apk/repositories'),'--no-network','fetch','--world','--recursive','--output',str(out)]
r=subprocess.run(cmd,capture_output=True);(p/'local-fetch.stdout').write_bytes(r.stdout);(p/'local-fetch.stderr').write_bytes(r.stderr);records.append(dict(label='local-fetch',argv=cmd,returncode=r.returncode));assert r.returncode==0
assert len(list(out.glob('*.apk')))==25
for f in files:assert hashlib.sha256(f.read_bytes()).digest()==hashlib.sha256((out/f.name).read_bytes()).digest()
for label,used,expected in [('all25',files,0),('missing-mkinitfs',[f for f in files if not f.name.startswith('mkinitfs-')],2)]:
 root=p/('add-simulation-'+label);root.mkdir()
 cmd=apk+['--root',str(root),'--arch','x86_64','--keys-dir',str(b/'etc/apk/keys'),'--repositories-file','/dev/null','--no-network','--no-scripts','--no-chown','--initdb','add','--simulate','linux-virt=6.12.114-r0']+[str(f) for f in used]
 r=subprocess.run(cmd,capture_output=True);(p/(label+'-add-simulate.stdout')).write_bytes(r.stdout);(p/(label+'-add-simulate.stderr')).write_bytes(r.stderr)
 state={str(f.relative_to(root)):{'bytes':f.stat().st_size,'sha256':hashlib.sha256(f.read_bytes()).hexdigest()} for f in root.rglob('*') if f.is_file()}
 records.append(dict(label=label,argv=cmd,returncode=r.returncode,root_files=state));assert r.returncode==expected,(label,r.returncode)
 assert all(v['bytes']==0 for v in state.values()),state
(p/'offline-controls.json').write_text(json.dumps(records,indent=2)+'\n');print([(x['label'],x['returncode'])for x in records])
