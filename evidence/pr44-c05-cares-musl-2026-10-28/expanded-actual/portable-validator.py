from pathlib import Path
import json,hashlib
p=Path(__file__).parent
H=lambda b:hashlib.sha256(b).hexdigest()
s=json.loads((p/'installed-union-security-summary.json').read_text())
def db(b):
 rows=[]
 for block in b.decode().strip().split('\n\n'):
  v=dict(line.split(':',1) for line in block.splitlines() if len(line)>2 and line[1]==':')
  if 'P'in v:rows.append((v['P'],v['V']))
 assert len(rows)==len(dict(rows));return dict(rows)
installed=(p/'installed.txt').read_bytes();bootstrap=(p/'bootstrap-installed.txt').read_bytes()
assert H(installed)==s['installed_db_sha256'] and H(bootstrap)==s['bootstrap_db_sha256']
identities=json.loads((p/'all160-signed-identities.json').read_text());expected=db(bootstrap)
assert len(identities)==160 and len({a['pkgname']for a in identities})==160
expected.update({a['pkgname']:a['pkgver']for a in identities});assert db(installed)==expected and len(expected)==168
manifest=(p/'file-manifest.json').read_bytes();assert H(manifest)==s['file_manifest_sha256'];rows=json.loads(manifest);assert len(rows)==s['file_manifest_count']==5477
index={a['path']:a for a in rows};assert len(index)==len(rows);assert index['lib/apk/db/installed']['sha256']==H(installed)
for path,meta in s['security'].items():
 assert all(index[path][key]==meta[key]for key in ['mode','uid','gid'])
 if 'capability_hex'in meta:assert index[path]['xattrs']['security.capability']==meta['capability_hex']==meta['signed_archive_capability_hex']
for path,meta in s['abi'].items():assert index[meta['realpath']]['sha256']==meta['sha256']
print('PASS portable exact168 union, bound5477-file manifest, recorded security/content observations; not independent native re-observation')
