#!/usr/bin/env python3
"""Actual host C forwarding/child-only wrapper tests; NOT actual Qt/SMP proof."""
from pathlib import Path
import os,shlex,subprocess,tempfile,unittest
TOP=Path(__file__).resolve().parents[1];SRC=TOP/'userland/tests/qt-key-observer'
class Test(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();cls.root=Path(cls.tmp.name)
  d=cls.root
  for name,out in [('qt-key-observer','observer'),('qt-key-observer-mock','mock')]:
   subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-D_FORTIFY_SOURCE=2','-shared','-fPIC',str(SRC/(name+'.c')),'-Wl,--version-script='+str(SRC/(name+'.map')),'-ldl','-o',str(d/('lib'+out+'.so'))],check=True)
  subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-D_FORTIFY_SOURCE=2',str(SRC/'qt-key-observer-caller.c'),'-L'+str(d),'-lmock','-Wl,-rpath,'+str(d),'-o',str(d/'caller')],check=True)
  cls.plain=cls.call(False);cls.observed=cls.call(True)
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 @classmethod
 def call(cls,trace):
  env={**os.environ};env.pop('LD_PRELOAD',None)
  if trace:env['LD_PRELOAD']=str(cls.root/'libobserver.so')
  return subprocess.run([str(cls.root/'caller')],env=env,capture_output=True,text=True,timeout=10)
 def test_all_forwarded_arguments_and_results(self):
  for r in [self.plain,self.observed]:self.assertEqual(r.returncode,0,r.stderr[:500]);self.assertEqual(r.stdout,'MOCK_ABI_FORWARDING_PASS calls=4100\n')
  self.assertEqual(self.plain.stderr,'')
  lines=self.observed.stderr.splitlines()
  for i in range(4096):
   for off,phase,result in [(0,'begin',-1),(1,'end',i&1)]:
    r=dict(kv.split('=',1)for kv in lines[1+i*2+off].split()[1:])
    self.assertEqual(r['seq'],str(i));self.assertEqual(r['phase'],phase)
    for k,v in {'source_ms':1234605616436508552,'type':6+(i&1),'key':68,'modifiers':33554432,'scan':40,'sym':100,'native':4275878552,'autorepeat':i&1,'count':65534,'result':result,'clock_ok':1}.items():self.assertEqual(r[k],str(v))
    self.assertGreater(int(r['pid']),0);self.assertGreater(int(r['mono_ns']),0)
 def test_explicit_bounded_output(self):
  lines=self.observed.stderr.splitlines();self.assertEqual(len(lines),8194)
  self.assertEqual(lines[0],'QT_KEY_TRACE_LOADED abi=Qt6.8.3-LP64 boundary=platform-submission')
  self.assertEqual(lines[-1],'QT_KEY_TRACE_TRUNCATED limit=4096')
  self.assertEqual(self.observed.stderr.count('QT_KEY_TRACE_TRUNCATED'),1)
 def test_non_qt_child_inherited_preload_is_quiet(self):
  r=subprocess.run(['/bin/true'],env={**os.environ,'LD_PRELOAD':str(self.root/'libobserver.so')},capture_output=True,timeout=5)
  self.assertEqual((r.returncode,r.stdout,r.stderr),(0,b'',b''))
 def test_real_wrapper_preserves_arguments_pid_parent_and_preload(self):
  d=self.root
  (d/'empty.c').write_text('int innocent_preload;\n')
  subprocess.run(['cc','-shared','-fPIC',str(d/'empty.c'),'-o',str(d/'empty.so')],check=True)
  wrapper=(TOP/'userland/alpine/desktop-overlay/usr/bin/konsole-key-trace').read_text().replace('/usr/lib/lib9os-qt-key-observer.so',str(d/'libobserver.so'))
  (d/'wrapper').write_text(wrapper);(d/'wrapper').chmod(0o755)
  (d/'konsole').write_text('#!/bin/sh\nprintf "pid=%s preload=%s debug=%s\\n" "$$" "$LD_PRELOAD" "${WAYLAND_DEBUG-unset}"\nprintf "arg=%s\\n" "$@"\n');(d/'konsole').chmod(0o755)
  env={**os.environ,'PATH':str(d)+':'+os.environ['PATH'],'LD_PRELOAD':str(d/'empty.so'),'WAYLAND_DEBUG':'server'}
  cmd=shlex.quote(str(d/'wrapper'))+' --separate "two words" & child=$!;wait "$child";printf "child=%s parent=%s\\n" "$child" "$LD_PRELOAD"\n'
  r=subprocess.run(['/bin/sh','-eu','-c',cmd],env=env,text=True,capture_output=True,timeout=5)
  self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stderr,'')
  first,a,b,last=r.stdout.splitlines();pid=first.split()[0].split('=')[1]
  self.assertEqual(first,'pid='+pid+' preload='+str(d/'libobserver.so')+':'+str(d/'empty.so')+' debug=server')
  self.assertEqual([a,b],['arg=--separate','arg=two words']);self.assertEqual(last,'child='+pid+' parent='+str(d/'empty.so'))
 def test_missing_wrapper_library_rejected(self):
  s=(TOP/'userland/alpine/desktop-overlay/usr/bin/konsole-key-trace').read_text().replace('/usr/lib/lib9os-qt-key-observer.so',str(self.root/'absent.so'))
  r=subprocess.run(['/bin/sh','-c',s],capture_output=True,text=True,timeout=5)
  self.assertEqual(r.returncode,2);self.assertIn('Missing Qt key observer library',r.stderr)
if __name__=='__main__':unittest.main()
