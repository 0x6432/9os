from pathlib import Path
import urllib.request,urllib.error,json,hashlib,datetime
r=Path('/data/pr44-current-ci-once');records=[];cap=15*1024*1024
requests=[('failed-job-122431334416.txt','https://nightly.link/0x6432/9os/runs/122431334416.txt'),('failed-job-122431399357.txt','https://nightly.link/0x6432/9os/runs/122431399357.txt'),('artifacts-40572840020.json','https://api.github.com/repos/0x6432/9os/actions/runs/40572840020/artifacts?per_page=100'),('artifacts-40572839971.json','https://api.github.com/repos/0x6432/9os/actions/runs/40572839971/artifacts?per_page=100')]
for name,url in requests:
 rec={'url':url,'name':name,'at_utc':datetime.datetime.now(datetime.timezone.utc).isoformat()}
 try:
  req=urllib.request.Request(url,headers={'Accept':'application/vnd.github+json','User-Agent':'PR44-authorized-completed-failure-preservation'})
  with urllib.request.urlopen(req,timeout=60)as z:
   rec.update(status=z.status,headers=dict(z.headers));size=z.headers.get('Content-Length')
   if size is not None and int(size)>cap:raise RuntimeError('Over 15MiB; stopped before body')
   b=z.read(cap+1)
  if len(b)>cap:raise RuntimeError('Over 15MiB; stopped')
  rec.update(bytes=len(b),sha256=hashlib.sha256(b).hexdigest());(r/name).write_bytes(b)
  print(json.dumps({k:rec[k]for k in ['name','status','bytes','sha256']}))
  if name.endswith('.json'):
   d=json.loads(b);print(json.dumps({'run_artifacts':name,'total':d['total_count'],'artifacts':[{k:a.get(k)for k in ['id','name','size_in_bytes','expired','digest']}for a in d['artifacts']]}))
 except urllib.error.HTTPError as e:rec.update(status=e.code,stop='HTTP error no retry');print('STOP',name,'HTTP',e.code);records.append(rec);break
 except Exception as e:rec['stop']=str(e);print('STOP',name,rec['stop']);records.append(rec);break
 records.append(rec)
(r/'originals-provenance.json').write_text(json.dumps(records,indent=2)+'\n')
