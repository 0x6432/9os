from pathlib import Path
import urllib.request,urllib.error,json,datetime,hashlib
r=Path('/data/pr44-current-ci-once');base='https://api.github.com/repos/0x6432/9os';head='c05a3fcd851297161f3981b38dd04900c49e8467';proof=[]
def get(path,name):
 url=base+path;req=urllib.request.Request(url,headers={'Accept':'application/vnd.github+json','User-Agent':'PR44-authorized-single-current-CI-snapshot'})
 with urllib.request.urlopen(req,timeout=30)as z:
  b=z.read(2*1024*1024+1);assert len(b)<=2*1024*1024;rec={'url':url,'status':z.status,'headers':dict(z.headers),'bytes':len(b),'sha256':hashlib.sha256(b).hexdigest(),'at_utc':datetime.datetime.now(datetime.timezone.utc).isoformat()};proof.append(rec)
 (r/name).write_bytes(b);return json.loads(b)
try:
 d=get('/actions/runs?head_sha='+head+'&per_page=30','runs.json')
 selected=[x for x in d['workflow_runs']if x['head_sha']==head and x['event']=='pull_request' and x['name']in ['alpine-prebuilt-runtime','build-and-release']]
 assert len(selected)<=2,'Unexpected duplicate automatic runs; stop before expansion'
 for candidate in selected:
  run=get('/actions/runs/'+str(candidate['id']),f'run-{candidate["id"]}.json');assert run['head_sha']==head
  jobs=get('/actions/runs/'+str(run['id'])+'/attempts/'+str(run['run_attempt'])+'/jobs?per_page=100',f'jobs-{run["id"]}.json')
  print(json.dumps({'run':{k:run.get(k)for k in ['id','name','head_sha','run_attempt','event','status','conclusion','html_url']},'jobs':[{k:j.get(k)for k in ['id','name','status','conclusion','html_url','started_at','completed_at','steps']}for j in jobs['jobs']]}))
 if not selected:print('No automatic signed/source runs found for exact head in this snapshot.')
except urllib.error.HTTPError as e:print('STOP HTTP',e.code,'no retry')
finally:(r/'metadata-provenance.json').write_text(json.dumps(proof,indent=2)+'\n')
