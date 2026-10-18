from pathlib import Path
import json,urllib.request,hashlib,subprocess,re
root=Path('/data/matched-elf');root.mkdir(exist_ok=True)
lock=json.loads(Path('/data/9os/userland/alpine/locks/x86_64-desktop.json').read_text())
wanted={'qt6-qtbase-6.8.3-r0.apk':r'usr/lib/libQt6Core\.so\.6\.8\.3','konsole-25.04.2-r0.apk':r'usr/lib/libkonsoleprivate\.so\.25\.04\.2','icu-libs-76.1-r1.apk':r'usr/lib/libicuuc\.so\.76\.1','breeze-6.3.6-r0.apk':r'usr/lib/qt6/plugins/styles/breeze6\.so','musl-1.2.5-r12.apk':r'lib/ld-musl-x86_64\.so\.1'}
manifest=[]
for p in lock['packages']:
 name=p['url'].rsplit('/',1)[-1]
 if name not in wanted or not name.startswith(('qt6-qtbase-', 'musl-')):continue
 a=root/name
 if not a.exists():
  with urllib.request.urlopen(p['url'])as r:a.write_bytes(r.read())
 assert hashlib.sha256(a.read_bytes()).hexdigest()==p['sha256']
 members=subprocess.check_output(['tar','--ignore-zeros','-tzf',str(a)],stderr=subprocess.DEVNULL,text=True).splitlines()
 chosen=[n for n in members if re.fullmatch(wanted[name],n.lstrip('./'))]
 assert len(chosen)==1,(name,chosen)
 b=subprocess.check_output(['tar','--ignore-zeros','-xOzf',str(a),chosen[0]],stderr=subprocess.DEVNULL);assert b.startswith(b'\x7fELF')
 out=root/chosen[0];out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(b)
 manifest.append({'package':p,'member':chosen[0],'elf_sha256':hashlib.sha256(b).hexdigest()});print('Exact locked ELF',chosen[0],len(b))
(root/'provenance.json').write_text(json.dumps(manifest,indent=2)+'\n')
