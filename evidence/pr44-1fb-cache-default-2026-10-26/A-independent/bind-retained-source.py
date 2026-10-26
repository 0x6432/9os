from pathlib import Path
import re,base64,hashlib,tarfile,json
root=Path('/data/pr44-oracle-cache-review')
ids=['f6fe7c41-c46b-40c5-98db-a1e14c34c92f','7ffc9a24-d0f5-44be-8391-db8ac2ec5d19','9a429f5d-53c8-4a85-9406-a61d20c8f67c','937c1050-8cfb-4878-b2f4-7bfe5f79bbf1','a08d26a7-7b56-4d88-b601-2b01b58abafe','45907392-7d3e-41b4-ba88-bb963d434b41']
chunks=[]
for n,i in enumerate(ids,1):
 p=Path('/data/.agent-service/files')/i/('tool-result' if n==6 else 'tool-result.txt')
 t=p.read_text(); assert f'chunk{n}/6' in t
 m=re.search(r'BASE64\\?_CHUNK:([A-Za-z0-9+/=]+)',t); assert m
 chunks.append(m[1])
b=base64.b64decode(''.join(chunks),validate=True)
s256=hashlib.sha256(b).hexdigest(); s512=hashlib.sha512(b).hexdigest()
assert len(b)==200862
assert s256=='62dcb1951108ba24c7478bbca402ea9a34b3bb09baa161b220d0a18e15645e35'
recipe=(root/'source/APKBUILD').read_text()
declared=re.search(r'([a-f0-9]{128})  apk-tools-v2\.14\.12\.tar\.gz',recipe)[1]
assert s512==declared
p=root/'apk-tools-v2.14.12.tar.gz';p.write_bytes(b)
matched=[]
with tarfile.open(p,'r:gz') as tf:
 for local in sorted((root/'source/selected-source').rglob('*')):
  if not local.is_file(): continue
  rel=local.relative_to(root/'source/selected-source').as_posix()
  ms=[m for m in tf.getmembers() if m.name.endswith('/'+rel)]
  assert len(ms)==1 and ms[0].isfile(),rel
  data=tf.extractfile(ms[0]).read(); assert data==local.read_bytes(),rel
  matched.append({'path':rel,'archive_member':ms[0].name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
assert len(matched)==9
result={'archive_bytes':len(b),'sha256':s256,'sha512':s512,'actual_APKBUILD_declared_sha512_match':True,'chunks':[len(c) for c in chunks],'selected_source_byte_matches':matched,'no_new_GET_or_source_execution':True}
(root/'independent-source-binding.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({'archive_bytes':len(b),'sha256':s256,'recipe_sha512_match':True,'selected_files_exact':len(matched),'chunk_lengths':[len(c) for c in chunks]}))
