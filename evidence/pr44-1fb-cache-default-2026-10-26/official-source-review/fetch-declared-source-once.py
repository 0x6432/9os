from pathlib import Path
import urllib.request,urllib.error,json,hashlib,datetime
p=Path(__file__).parent;used=(p/'APKBUILD').stat().st_size;cap=20*1024*1024;url='https://gitlab.alpinelinux.org/alpine/apk-tools/-/archive/v2.14.12/apk-tools-v2.14.12.tar.gz';expected='d319980f7d57246ad885202736bb20dd88f031f7e8b058f612fd5df1a5e88387522f599b5a03ab4507b51385675cadd67c6fcf50817ce6dcfdd410d2be5aa5fc'
try:r=urllib.request.urlopen(url,timeout=45)
except urllib.error.HTTPError as error:r=error
with r:
 length=int(r.headers.get('Content-Length','0'));assert length<=cap-used,'Over aggregate cap';raw=r.read(cap-used);assert len(raw)<cap-used,'Aggregate cap reached';meta={'url':url,'final_url':r.url,'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'status':r.status,'headers':dict(r.headers),'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),'sha512':hashlib.sha512(raw).hexdigest(),'declared_sha512':expected,'aggregate_downloaded_bytes':used+len(raw),'hard_cap_bytes':cap}
(p/'source-response.bin').write_bytes(raw);(p/'source-provenance.json').write_text(json.dumps(meta,indent=2)+'\n');print(json.dumps({k:v for k,v in meta.items()if k!='headers'}));assert meta['status']==200,'Source request failed: stop without retry/fallback';assert meta['sha512']==expected,'Declared source digest mismatch: stop';(p/'apk-tools-v2.14.12.tar.gz').write_bytes(raw)
