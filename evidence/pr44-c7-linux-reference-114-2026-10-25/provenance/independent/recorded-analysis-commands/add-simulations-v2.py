import pathlib,subprocess,json,hashlib
p=pathlib.Path(__file__).parent;b=p/'bootstrap';files=sorted((p/'local-repository/x86_64').glob('*.apk'))
apk=[str(b/'lib/ld-musl-x86_64.so.1'),'--library-path',f'{b}/lib:{b}/usr/lib',str(b/'sbin/apk')]
records=[]
for label,used,expected in [('all25',files,0),('missing-mkinitfs',[f for f in files if not f.name.startswith('mkinitfs-')],2)]:
 root=p/('add-simulation-v2-'+label);root.mkdir()
 for name in ['etc/apk/world','lib/apk/db/installed','lib/apk/db/triggers']:
  f=root/name;f.parent.mkdir(parents=True,exist_ok=True);f.touch()
 def state():return {str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in root.rglob('*') if f.is_file()}
 before=state()
 cmd=apk+['--root',str(root),'--arch','x86_64','--keys-dir',str(b/'etc/apk/keys'),'--repositories-file','/dev/null','--no-network','--no-scripts','--no-chown','add','--simulate','linux-virt=6.12.114-r0']+[str(f) for f in used]
 r=subprocess.run(cmd,capture_output=True);(p/(label+'-add-simulate-v2.stdout')).write_bytes(r.stdout);(p/(label+'-add-simulate-v2.stderr')).write_bytes(r.stderr)
 records.append(dict(label=label,argv=cmd,returncode=r.returncode,before=before,after=state()));assert r.returncode==expected,(label,r.returncode,r.stderr)
 assert before==state()
(p/'add-simulations-v2.json').write_text(json.dumps(records,indent=2)+'\n');print([(x['label'],x['returncode'])for x in records])
