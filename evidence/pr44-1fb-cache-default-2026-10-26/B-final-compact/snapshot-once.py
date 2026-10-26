import urllib.request,json,pathlib,hashlib,datetime
p=pathlib.Path(__file__).parent/'originals';base='https://api.github.com/repos/0x6432/9os/actions/runs/40217589621';meta=[]
for name,url in [('run.json',base),('jobs.json',base+'/attempts/1/jobs?per_page=100')]:
 req=urllib.request.Request(url,headers={'Accept':'application/vnd.github+json','User-Agent':'9os-readonly-ci-review'})
 with urllib.request.urlopen(req,timeout=30)as r:b=r.read();m={'name':name,'url':url,'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'status':r.status,'headers':dict(r.headers),'bytes':len(b),'sha256':hashlib.sha256(b).hexdigest()}
 (p/name).write_bytes(b);meta.append(m)
(p/'snapshot-provenance.json').write_text(json.dumps(meta,indent=2)+'\n');run=json.loads((p/'run.json').read_bytes());jobs=json.loads((p/'jobs.json').read_bytes())['jobs'];print('RUN',run['head_sha'],run['run_attempt'],run['status'],run['conclusion']);print('JOBS',len(jobs))
for j in jobs:
 print(json.dumps({'id':j['id'],'name':j['name'],'status':j['status'],'conclusion':j['conclusion'],'started_at':j['started_at'],'completed_at':j['completed_at'],'steps':[{'n':s['number'],'name':s['name'],'status':s['status'],'conclusion':s['conclusion']}for s in j['steps']]}))
