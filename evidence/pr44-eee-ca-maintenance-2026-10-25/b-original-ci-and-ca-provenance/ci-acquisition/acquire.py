import json,pathlib,urllib.request,hashlib,concurrent.futures,zipfile
p=pathlib.Path('/data/signed-eee-review/originals');jobs=json.loads((p/'jobs.json').read_bytes())['jobs'];arts=json.loads((p/'artifacts.json').read_bytes())['artifacts'];urls=[(f"job-{j['id']}.txt",f"https://nightly.link/0x6432/9os/runs/{j['id']}.txt") for j in jobs]+[(a['name']+'.zip','https://nightly.link/0x6432/9os/actions/runs/40208715179/'+a['name']+'.zip') for a in arts]
def get(pair):
 name,url=pair
 try:
  with urllib.request.urlopen(url,timeout=60) as r:b=r.read()
  (p/name).write_bytes(b)
  if name.endswith('.zip'):
   with zipfile.ZipFile(p/name) as z:assert z.testzip() is None
  return {'file':name,'url':url,'bytes':len(b),'sha256':hashlib.sha256(b).hexdigest()}
 except Exception as e:return {'file':name,'url':url,'error':repr(e)}
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:rows=list(ex.map(get,urls))
pathlib.Path('/data/signed-eee-review/acquisition.json').write_text(json.dumps(rows,indent=2)+'\n')
for r in rows:print(json.dumps(r))
