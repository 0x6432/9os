import urllib.request,hashlib,json,pathlib,time,datetime
p=pathlib.Path(__file__).parent
u='https://dl-cdn.alpinelinux.org/alpine/v3.22/main/x86_64/ca-certificates-bundle-20261020-r0.apk'
t=time.monotonic(); started=datetime.datetime.now(datetime.timezone.utc).isoformat()
with urllib.request.urlopen(u,timeout=60) as r:
 b=r.read(); meta={'url':u,'utc_start':started,'status':r.status,'final_url':r.url,'size':len(b),'sha256':hashlib.sha256(b).hexdigest(),'elapsed_s':round(time.monotonic()-t,3),'response_headers':dict(r.headers)}
(p/'acquisition.json').write_text(json.dumps(meta,indent=2)+'\n');(p/'ca-certificates-bundle-20261020-r0.apk').write_bytes(b)
assert meta['size']==107116 and meta['sha256']=='d35216a0a9adde79031df246759bad03f235cf201bf0aff8436b88bff59594eb'
print(json.dumps({k:v for k,v in meta.items() if k!='response_headers'}))
