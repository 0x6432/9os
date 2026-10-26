from pathlib import Path
import urllib.request,json,hashlib,datetime
p=Path(__file__).parent;url='https://raw.githubusercontent.com/alpinelinux/aports/a53adea12086d36717300ea195ede9ad75f98e1f/main/apk-tools/APKBUILD';cap=20*1024*1024
with urllib.request.urlopen(url,timeout=30)as r:
 assert int(r.headers.get('Content-Length','0'))<=cap
 raw=r.read(cap)
 assert len(raw)<cap
 meta={'url':url,'status':r.status,'headers':dict(r.headers),'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),'sha512':hashlib.sha512(raw).hexdigest(),'aggregate_cap_bytes':cap}
(p/'APKBUILD').write_bytes(raw);(p/'APKBUILD-provenance.json').write_text(json.dumps(meta,indent=2)+'\n');print('APKBUILD',len(raw),meta['sha256']);print(raw.decode())
