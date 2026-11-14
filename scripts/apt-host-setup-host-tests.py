#!/usr/bin/env python3
"""Execute actual YAML run blocks through native Bash with recording mocks.
No apt/network/install/QEMU is run. This proves argument plumbing and fail-closed
step sequencing only, NOT real network recovery or an absolute time bound.
"""
import json, os, pathlib, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
OPTIONS={'Acquire::http::Timeout':'30','Acquire::https::Timeout':'30','Acquire::Retries':'2'}
class Controls(unittest.TestCase):
 def block(self,filename,name):
  # Extract the actual literal YAML command block, not a copied approximation.
  # Deliberately strict: a renamed step or changed block style needs review.
  lines=(ROOT/filename).read_text().splitlines()
  marker='      - name: '+name
  self.assertEqual(lines.count(marker),1,(filename,name))
  i=lines.index(marker)+1
  while i<len(lines) and lines[i].startswith('        ') and lines[i]!='        run: |':i+=1
  self.assertLess(i,len(lines));self.assertEqual(lines[i],'        run: |')
  out=[]
  for line in lines[i+1:]:
   if line and not line.startswith('          '):break
   out.append(line[10:] if line else '')
  self.assertTrue(out)
  return {'name':name,'run':'\n'.join(out)+'\n'}
 def configs(self):
  alpine='.github/workflows/alpine.yml';release='.github/workflows/release.yml'
  for job,name in [('alpine','Host tools only (no Qt compiler/toolchain build)'),('desktop','Host tools only; signed prebuilt desktop dependencies')]:
   yield alpine,job,[self.block(alpine,name)]
  yield release,'build',[self.block(release,name) for name in ['Install host packages','Install QEMU (Alpine edge, same as the dev sandbox)','Fetch Limine + uACPI']]
 def execute(self,steps,fail):
  with tempfile.TemporaryDirectory(prefix='apt-command-control-') as td:
   d=pathlib.Path(td);(d/'bin').mkdir();(d/'scripts').mkdir()
   mock='''#!/usr/bin/env python3
import os,sys,json
from pathlib import Path
name=Path(sys.argv[0]).name;args=sys.argv[1:]
with open(os.environ['MOCK_LOG'],'a') as f:f.write(json.dumps([name,args])+'\\n')
if name=='apt-get':
 action='update' if 'update' in args else 'install'
 if action==os.environ['MOCK_FAIL']:sys.exit(100 if action=='update' else 101)
 if action=='update' and os.environ['MOCK_FAIL']=='partial-index':
  sys.exit(100 if 'APT::Update::Error-Mode=any' in args else 0)
if name=='clang':print('mock clang')
'''
   for name in ['apt-get','pip3','clang']:
    p=d/'bin'/name;p.write_text(mock);p.chmod(0o755)
   p=d/'bin/sudo';p.write_text('#!/bin/sh\nexec "$@"\n');p.chmod(0o755)
   for name in ['install-qemu-alpine.sh','fetch-deps.sh']:
    p=d/'scripts'/name;p.write_text(mock);p.chmod(0o755)
   env={**os.environ,'PATH':str(d/'bin')+':'+os.environ['PATH'],'MOCK_LOG':str(d/'calls.jsonl'),'MOCK_FAIL':fail,'TOOLS_DIR':str(d/'tools'),'GITHUB_PATH':str(d/'github-path')}
   rc=0;ran=[]
   for step in steps:
    p=subprocess.run(['/bin/bash','-e','-o','pipefail','-c',step['run']],cwd=d,env=env,capture_output=True,timeout=5)
    ran.append(step['name']);rc=p.returncode
    if rc:break
   calls=[json.loads(x) for x in (d/'calls.jsonl').read_text().splitlines()]
   return rc,calls,ran
 def assert_args(self,calls):
  for name,args in calls:
   if name!='apt-get':continue
   opts={}
   for i,a in enumerate(args):
    if a=='-o':k,v=args[i+1].split('=',1);opts[k]=v
   for k,v in OPTIONS.items():self.assertEqual(opts.get(k),v,(name,args))
   if 'update' in args:self.assertEqual(opts.get('APT::Update::Error-Mode'),'any')
   self.assertNotIn('--allow-unauthenticated',args)
 def test_host_gates_include_setup_and_download_controls_once(self):
  for filename,name in [('.github/workflows/alpine.yml','Lock/checksum negative controls'),('.github/workflows/release.yml','Allocator and scheduler host regressions')]:
   with self.subTest(workflow=filename):
    block=self.block(filename,name)['run']
    for test in ['apt-host-setup-host-tests.py','alpine-download-host-tests.py']:
     self.assertEqual(block.count('python3 scripts/'+test),1)
 def test_actual_blocks_fail_closed_and_preserve_sequence(self):
  for fn,job,steps in self.configs():
   for fail,expected in [('update',100),('partial-index',100),('install',101),('',0)]:
    with self.subTest(workflow=fn,job=job,scenario=fail or 'success'):
     rc,calls,ran=self.execute(steps,fail);self.assertEqual(rc,expected);self.assert_args(calls)
     names=[n for n,a in calls]
     apt=[a for n,a in calls if n=='apt-get']
     self.assertIn('update',apt[0])
     if fail in ['update','partial-index']:
      self.assertEqual(len(apt),1);self.assertEqual(names,['apt-get'])
     elif fail=='install':
      self.assertEqual(len(apt),2);self.assertIn('install',apt[1]);self.assertEqual(names,['apt-get','apt-get'])
     else:
      self.assertEqual(len(apt),2);self.assertIn('install',apt[1])
      self.assertIn('install-qemu-alpine.sh',names);self.assertIn('fetch-deps.sh',names)
      self.assertLess(names.index('install-qemu-alpine.sh'),names.index('fetch-deps.sh'))
     print(json.dumps({'workflow':fn,'job':job,'scenario':fail or 'success','rc':rc,'steps_run':ran,'calls':calls}),flush=True)
if __name__=='__main__':unittest.main(verbosity=2)
