#!/usr/bin/env python3
"""Read-only validator of retained capture files; does not repeat installation.
This validates internal recorded identities/content consistency, not a new
observation of the live root, package signature run or security xattr.
"""
import argparse,hashlib,json,pathlib
ap=argparse.ArgumentParser();ap.add_argument('directory',type=pathlib.Path);e=ap.parse_args().directory
load=lambda name:json.loads((e/name).read_text())
def parse_db(raw):
 result={}
 for block in raw.split('\n\n'):
  fields={}
  for line in block.splitlines():
   if len(line)>2 and line[1]==':':fields[line[0]]=line[2:]
  if 'P'in fields:
   assert fields['P']not in result
   result[fields['P']]=fields
 return result
mini_raw=(e/'native-bootstrap-installed-db').read_bytes();assert len(mini_raw)==15306
assert hashlib.sha256(mini_raw).hexdigest()=='74c01562aa75d8dd6dbb5c8b30f2d2d73c7d445146335962cc68930d751ddab1'
mini=parse_db(mini_raw.decode());installed=parse_db((e/'native-installed-db').read_text());inputs=load('native-all160-verified-metadata.json');manifest=load('native-root-manifest.json');assert len(inputs)==160
assert sorted([{'url':x['url'],'sha256':x['sha256']}for x in inputs],key=lambda x:x['url'])==manifest['packages']
expected={name:data['V']for name,data in mini.items()};names=set()
for x in inputs:
 d=x['pkginfo'];name=d['pkgname'][0];assert name not in names;names.add(name);expected[name]=d['pkgver'][0]
assert {name:data['V']for name,data in installed.items()}==expected
assert len(mini)==16 and len(installed)==168
assert installed['ca-certificates-bundle']['V']=='20261020-r0'
raw=(e/'native-root-files-manifest.json').read_bytes();assert len(raw)==922236
assert hashlib.sha256(raw).hexdigest()=='65dd2097aed9919ba05ea311fdf1d9226406b008d40464496e063a53e707e37d'
records=json.loads(raw);assert len(records)==5477;files={x['path']:x for x in records};assert len(files)==len(records)
assert files['etc/ssl/certs/ca-certificates.crt']['sha256']=='5961c0a3c65459931c023e0075c09733f09aea4ca439cb85bc4a08bb4f3f568b'
assert files['bin/busybox']['mode']=='0o4755'
assert files['etc/ssl/cert.pem']['symlink']=='certs/ca-certificates.crt'
assert files['etc/9os-alpine.json']['sha256']==hashlib.sha256((e/'native-root-manifest.json').read_bytes()).hexdigest()
security=load('native-installed-security-and-dependencies.json');assert security['sway_security_capability_hex']=='0100000200008000000000000000000000000000'
assert security['apk_signature_OK_lines']==160
print('PASS retained identities: 160 locked + 8 bootstrap-only =168; 5477-file recorded manifest; CA hash/symlink, BusyBox mode, recorded Sway capability consistent')
print('No new root observation, signature run, installation, boot or CI claim')
