import urllib.request,json,pathlib,hashlib,datetime,concurrent.futures,zipfile
b=pathlib.Path(__file__).parent;p=b/'originals';u='https://api.github.com/repos/0x6432/9os/actions/runs/40217589621/artifacts?per_page=100';req=urllib.request.Request(u,headers={'Accept':'application/vnd.github+json','User-Agent':'9os-readonly-ci-review'})
with urllib.request.urlopen(req,timeout=30)as r:raw=r.read();m={'url':u,'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'status':r.status,'headers':dict(r.headers),'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()}
(p/'artifacts.json').write_bytes(raw);(p/'artifacts-provenance.json').write_text(json.dumps(m,indent=2)+'\n');jobs=json.loads((p/'jobs.json').read_bytes())['jobs'];arts=json.loads(raw)['artifacts'];pairs=[(f"job-{j['id']}.txt",f"https://nightly.link/0x6432/9os/runs/{j['id']}.txt")for j in jobs if j['status']=='completed']+[(a['name']+'.zip','https://nightly.link/0x6432/9os/actions/runs/40217589621/'+a['name']+'.zip')for a in arts]
def get(pair):
 name,url=pair;started=datetime.datetime.now(datetime.timezone.utc).isoformat()
 try:
  with urllib.request.urlopen(url,timeout=60)as r:raw=r.read();meta={'file':name,'url':url,'final_url':r.url,'utc_start':started,'status':r.status,'headers':dict(r.headers),'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()}
  (p/name).write_bytes(raw)
  if name.endswith('.zip'):
   with zipfile.ZipFile(p/name)as z:assert z.testzip()is None
   meta['zip_crc']='PASS'
  return meta
 except Exception as error:return {'file':name,'url':url,'error':repr(error)}
with concurrent.futures.ThreadPoolExecutor(max_workers=4)as pool:results=list(pool.map(get,pairs))
(b/'acquisition.json').write_text(json.dumps(results,indent=2)+'\n')
for row in results:print(json.dumps({k:v for k,v in row.items()if k!='headers'}))
assert not any('error'in row for row in results)
