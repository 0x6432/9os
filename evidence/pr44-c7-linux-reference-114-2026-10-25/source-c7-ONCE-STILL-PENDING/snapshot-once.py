import pathlib,urllib.request,json,hashlib,datetime
p=pathlib.Path(__file__).parent;api='https://api.github.com/repos/0x6432/9os/actions/runs/40199695402';records=[]
def get(label,url):
 req=urllib.request.Request(url,headers={'User-Agent':'PR44-independent-readonly-review','Accept':'application/vnd.github+json'})
 with urllib.request.urlopen(req,timeout=60)as r:data=r.read();headers=str(r.headers);status=r.status
 (p/(label+'.json')).write_bytes(data);(p/(label+'.headers')).write_text(headers);records.append(dict(label=label,url=url,status=status,bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),acquired_at=datetime.datetime.now(datetime.timezone.utc).isoformat()));return json.loads(data)
r=get('run',api);j=get('jobs',api+'/jobs?per_page=100');(p/'acquisition.json').write_text(json.dumps(records,indent=2)+'\n')
assert r['head_sha']=='c7b434183c3e138b3b638922deb116f030d82282'and r['run_attempt']==1
print('SOURCE ONCE',r['status'],r['conclusion'],'exacthead',r['head_sha'],'attempt',r['run_attempt']);assert j['total_count']==len(j['jobs'])
for x in j['jobs']:print(x['id'],x['name'],x['status'],x['conclusion'])
(p/'summary.json').write_text(json.dumps(dict(run={k:r[k]for k in ['id','head_sha','run_attempt','status','conclusion','updated_at','html_url']},jobs=[{k:x[k]for k in ['id','name','status','conclusion','html_url']}for x in j['jobs']]),indent=2)+'\n')
