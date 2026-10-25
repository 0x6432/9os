import concurrent.futures,urllib.request,json,hashlib,datetime,pathlib
p=pathlib.Path('/data/pr44-ca-review'); rows=json.loads((p/'ci-compact/ci-acquisition/acquisition.json').read_text());o=p/'reacquired-ci';o.mkdir(exist_ok=False)
def fetch(x):
 start=datetime.datetime.now(datetime.timezone.utc).isoformat()
 with urllib.request.urlopen(x['url'],timeout=90) as r:
  data=r.read();status=r.status;headers=list(r.headers.items());final=r.url
 (o/x['file']).write_bytes(data)
 result=dict(x,observed_bytes=len(data),observed_sha256=hashlib.sha256(data).hexdigest(),current_http_status=status,current_response_headers=headers,current_final_url=final,current_started_at=start,provenance='NEW reacquisition HTTP observations; B original HTTP status/headers unavailable')
 (o/(x['file']+'.reacquisition.json')).write_text(json.dumps(result,indent=2)+'\n')
 assert len(data)==x['bytes'] and hashlib.sha256(data).hexdigest()==x['sha256'],x['file']
 return {'file':x['file'],'bytes':len(data),'sha256':result['observed_sha256'],'exact_original_bytes':True}
with concurrent.futures.ThreadPoolExecutor(max_workers=5) as ex: results=list(ex.map(fetch,rows))
(o/'verified-originals.json').write_text(json.dumps(results,indent=2)+'\n');print('Verified',len(results),'fixed original files;',sum(x['bytes'] for x in results),'bytes; no CI status or package requests')
