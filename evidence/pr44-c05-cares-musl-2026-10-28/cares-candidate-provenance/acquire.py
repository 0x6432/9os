import urllib.request, urllib.parse, json, hashlib, datetime, pathlib, sys
name,url,cap,expected_size,expected_sha=sys.argv[1:]; cap=int(cap)
out=pathlib.Path('/data/cares-provenance'); meta={'url':url,'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'request_count':1,'timeout':60,'cap':cap,'request_method':'GET','request_headers':'urllib defaults; no manual UA/proxy overrides'}
try:
 with urllib.request.urlopen(url, timeout=60) as r:
  meta.update(status=r.status,effective_url=r.url,headers=list(r.headers.items()))
  if urllib.parse.urlparse(r.url).hostname!='dl-cdn.alpinelinux.org':raise ValueError('unexpected effective source')
  data=r.read(cap+1)
  if len(data)>cap:raise ValueError('over cap')
  meta.update(size=len(data),sha256=hashlib.sha256(data).hexdigest())
  (out/name).write_bytes(data)
  if expected_size!='-' and len(data)!=int(expected_size):raise ValueError('size mismatch')
  if expected_sha!='-' and meta['sha256']!=expected_sha:raise ValueError('sha mismatch')
  meta['result']='PASS'
except Exception as e:
 meta.update(result='STOP',error_class=type(e).__name__,error=str(e));raise
finally:
 meta['finished_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat();(out/(name+'.http.json')).write_text(json.dumps(meta,indent=2)+'\n');print(json.dumps({k:meta[k] for k in ['url','result','status','size','sha256'] if k in meta}))
