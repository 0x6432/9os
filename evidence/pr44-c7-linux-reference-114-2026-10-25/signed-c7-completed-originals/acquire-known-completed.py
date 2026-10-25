import pathlib,urllib.request,hashlib,json,datetime,shutil
p=pathlib.Path(__file__).parent
items=[('121188042833',177861,'95f455cb396d07088d62acaf9c03560c34c2ef72da31a95e0ada25ac24f78394'),('121188042960',380171,'fae6f16b306a2356399ae735afb0d363a51437f9d25a68a9c3e50772454b263e'),('121188042966',1906716,'b228273e19d19f3acb8305d40bc1838b1ef20eaa9810cb1595a368980048fb18'),('121188043031',1912330,'c2f6719c81ea85681d55f25d5792dc8730c7b59a285ef5e64b4c0b9b6ac9d4dc'),('121188043130',615906,'a86f29159ce0b426e97e1221ec724fb698792f10555b8e477ffe3c7732447d7b'),('121188043150',2148951,'b221e32cbd1bb51dc9220d59d1790151040d06fbced49f54c9199a350d13df26')]
records=[]
for job,size,sha in items:
 dst=p/(job+'.txt');url='https://nightly.link/0x6432/9os/runs/'+job+'.txt'
 if dst.exists():raise SystemExit('Refuse overwrite')
 if job=='121188042833':
  shutil.copyfile('/data/pr44-c7-oracle-independent/job-original.txt',dst);provenance='Previously acquired full original reused; no new GET'
 else:
  with urllib.request.urlopen(url,timeout=60)as r:data=r.read();headers=str(r.headers);status=r.status
  dst.write_bytes(data);(p/(job+'.headers')).write_text(headers);provenance='Fresh GET of known completed original; no run/job status request'
 data=dst.read_bytes();got=hashlib.sha256(data).hexdigest();records.append(dict(url=url,bytes=len(data),sha256=got,expected_bytes=size,expected_sha256=sha,provenance=provenance,acquired_or_reused_at=datetime.datetime.now(datetime.timezone.utc).isoformat()))
 (p/'acquisition.json').write_text(json.dumps(records,indent=2)+'\n');assert len(data)==size and got==sha,job
 print(job,len(data),'SHA MATCH',flush=True)
