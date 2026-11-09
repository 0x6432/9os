#!/usr/bin/env python3
"""Exercise frozen launcher construction with inert files, never a real VM."""
from importlib.machinery import SourceFileLoader
import hashlib
import json
import os
import shutil
import signal
import time
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
FROZEN = SourceFileLoader('qemu_frozen', str(ROOT / 'scripts/qemu-frozen.py')).load_module()
FAKE = '''#!/usr/bin/python3
import os, re, sys
open(os.environ['FAKE_CALLED'],'w').write(str(os.getpid()))
if os.environ.get('FAKE_ARGV'): open(os.environ['FAKE_ARGV'],'w').write(__import__('json').dumps(sys.argv[1:]))
if os.environ.get('FAKE_MUTATE'):
 open(os.environ['FAKE_MUTATE'],'wb').write(b'changed after preflight')
print('Welcome to 9os\\n# ',end='',flush=True)
for line in sys.stdin:
 if line.strip() == 'poweroff':
  if os.environ.get('FAKE_POWER'): open(os.environ['FAKE_POWER'],'w').write('poweroff')
  break
 m = re.search(r'__RC(\\d+)=', line)
 if '__FROZENROOT=' in line: print('\\n__FROZENROOT=%s\\n# ' % ('1' if os.environ.get('FAKE_ROOT_FAIL') else '0'),end='',flush=True)
 elif m: print('FAKE CONTROL ONLY\\n__RC%s=%s\\n# ' % (m.group(1), '1' if os.environ.get('FAKE_RC_FAIL') else '0'),end='',flush=True)
 else: print('# ',end='',flush=True)
'''


class Fixture:
    def __init__(self, root):
        self.root = root
        self.qemu = root / 'inert-qemu'; self.qemu.write_text(FAKE); self.qemu.chmod(0o755)
        self.iso = root / 'inert.iso'; self.iso.write_bytes(b'INERT ISO')
        self.golden = root / 'golden.raw'; self.golden.write_bytes(b'INERT DISK')
        self.disk = root / 'disposable.raw'; self.disk.write_bytes(b'INERT DISK')
        self.monitor = str(root / 'monitor.sock'); self.qmp = str(root / 'qmp.sock')
        def entry(p):return {'path':str(p), 'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
        self.data = {'version':1, 'qemu':entry(self.qemu), 'iso':entry(self.iso), 'golden_disk':entry(self.golden)}
        self.manifest = root / 'manifest.json'; self.save()
    def save(self):self.manifest.write_text(json.dumps(self.data))
    def load(self):return FROZEN.load(str(self.manifest), str(self.disk), self.monitor, self.qmp)


class Frozen(unittest.TestCase):
    def test_exact_argv_no_make_and_frozen_hashes(self):
        with tempfile.TemporaryDirectory() as td:
            f = Fixture(Path(td)); cmd, state = f.load()
            self.assertEqual(cmd, [str(f.qemu), '-M', 'q35', '-cpu', 'qemu64,+smep,+smap,+rdrand',
                '-m', '3072M', '-smp', '4', '-serial', 'stdio', '-no-reboot',
                '-device', 'virtio-keyboard-pci', '-device', 'virtio-tablet-pci',
                '-netdev', 'user,id=n0', '-device', 'virtio-net-pci,netdev=n0',
                '-boot', 'd', '-cdrom', str(f.iso), '-audiodev', 'none,id=snd0',
                '-device', 'virtio-sound-pci,audiodev=snd0,streams=1', '-display', 'none',
                '-monitor', 'unix:'+f.monitor+',server,nowait', '-qmp', 'unix:'+f.qmp+',server,nowait',
                '-drive', 'file='+str(f.disk)+',if=none,id=vd0,format=raw',
                '-device', 'virtio-blk-pci,drive=vd0,serial=9os-disk0'])
            f.disk.write_bytes(b'guest writes only to disposable disk')
            FROZEN.verify_after(state)
            f.golden.write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'golden_disk changed'):FROZEN.verify_after(state)

    def test_offline_argv_removes_guest_network(self):
        with tempfile.TemporaryDirectory() as td:
            f=Fixture(Path(td))
            cmd,_=FROZEN.load(str(f.manifest),str(f.disk),f.monitor,f.qmp,offline=True)
            normal,_=f.load()
            expected=normal.copy()
            at=expected.index('-netdev')
            self.assertEqual(expected[at:at+4],['-netdev','user,id=n0','-device','virtio-net-pci,netdev=n0'])
            expected[at:at+4]=['-nic','none']
            self.assertEqual(cmd,expected)
            self.assertEqual(cmd.count('-nic'),1)
            self.assertNotIn('-netdev',cmd);self.assertNotIn('-net',cmd)
            self.assertFalse(any('virtio-net' in arg for arg in cmd))

    def test_offline_refuses_bad_hash_alias_and_socket(self):
        for case in ('hash','alias','socket'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as td:
                f=Fixture(Path(td))
                if case=='hash': f.data['iso']['sha256']='0'*64; f.save()
                if case=='alias': f.disk.unlink(); os.link(f.golden,f.disk)
                if case=='socket': Path(f.monitor).write_text('occupied')
                with self.assertRaises(ValueError):
                    FROZEN.load(str(f.manifest),str(f.disk),f.monitor,f.qmp,offline=True)

    def test_offline_option_requires_frozen_mode(self):
        r=subprocess.run(['python3',str(ROOT/'scripts/qemu-test.py'),'x86_64',
                          '--frozen-offline','echo harmless'],capture_output=True,text=True,timeout=10)
        self.assertNotEqual(r.returncode,0)
        self.assertIn('FAIL   frozen preflight',r.stdout)
        self.assertIn('SKIP   echo harmless',r.stdout)

    def test_offline_runner_passes_no_network_to_inert_executable(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);f=Fixture(root)
            env=dict(os.environ,FAKE_CALLED=str(root/'fake-called'),FAKE_ARGV=str(root/'argv'))
            for key in ('QEMU_EXTRA','FAKE_MUTATE','FAKE_ROOT_FAIL','FAKE_RC_FAIL'): env.pop(key,None)
            args=['python3',str(ROOT/'scripts/qemu-test.py'),'x86_64','--userland','alpine','--memory','3072M',
                  '--disk',str(f.disk),'--cmdline','root=/dev/vda1','--boot-timeout','300','--timeout','780',
                  '--frozen-manifest',str(f.manifest),'--frozen-offline','--monitor-socket',f.monitor,
                  '--qmp-socket',f.qmp,'--log',str(root/'run.log'),'echo harmless']
            r=subprocess.run(args,env=env,capture_output=True,text=True,timeout=30)
            self.assertEqual(r.returncode,0,r.stdout+r.stderr)
            cmd=json.loads((root/'argv').read_text())
            self.assertEqual(cmd[cmd.index('-nic')+1],'none')
            self.assertNotIn('-netdev',cmd);self.assertNotIn('-net',cmd)
            self.assertFalse(any('virtio-net' in arg for arg in cmd))
            self.assertIn('PASS   echo harmless',r.stdout)
            self.assertIn('FROZEN_NETWORK_MODE=no-guest-nic',r.stdout)
            self.assertIn('FROZEN_NETWORK_MODE=no-guest-nic',(root/'run.log').read_text())

    def test_preflight_rejects_changed_aliases_and_bad_manifest(self):
        with tempfile.TemporaryDirectory() as td:
            f = Fixture(Path(td))
            f.disk.write_bytes(b'bad')
            with self.assertRaisesRegex(ValueError, 'does not match'):f.load()
            f.disk.unlink();os.link(f.golden, f.disk)
            with self.assertRaisesRegex(ValueError, 'aliases'):f.load()
            f.disk.unlink();f.disk.symlink_to(f.golden)
            with self.assertRaisesRegex(ValueError, 'non-symlink'):f.load()
            f.disk.unlink();f.disk.write_bytes(b'INERT DISK')
            f.data['iso']['sha256']='0'*64;f.save()
            with self.assertRaisesRegex(ValueError, 'iso hash mismatch'):f.load()
            f.data['iso']['sha256']=hashlib.sha256(f.iso.read_bytes()).hexdigest();f.save()
            f.manifest.write_text('{"version":1,"version":1}')
            with self.assertRaisesRegex(ValueError, 'duplicate manifest key'):f.load()
            f.save();Path(f.monitor).write_text('occupied')
            with self.assertRaisesRegex(ValueError, 'fresh socket'):f.load()

    def test_unsafe_paths_and_options_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            f=Fixture(Path(td));f.data['iso']['path']='/tmp/unsafe,name';f.save()
            with self.assertRaisesRegex(ValueError, 'safe absolute'):f.load()
            f.data['iso']['path']=str(f.iso);f.data['version']=True;f.save()
            with self.assertRaisesRegex(ValueError, 'manifest shape'):f.load()

    def run_runner(self, mutate=False, root_fail=False, offline=False):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); f=Fixture(root); fakebin=root/'bin';fakebin.mkdir()
            maker=fakebin/'make';maker.write_text('#!/bin/sh\ntouch "$MAKE_CALLED"\nexit 77\n');maker.chmod(0o755)
            env=dict(os.environ, PATH=str(fakebin)+':/usr/bin:/bin', FAKE_CALLED=str(root/'fake-called'), MAKE_CALLED=str(root/'make-called'))
            env.pop('QEMU_EXTRA',None)
            if root_fail:env['FAKE_ROOT_FAIL']='1'
            else:env.pop('FAKE_ROOT_FAIL',None)
            if mutate:env['FAKE_MUTATE']=str(f.golden)
            else:env.pop('FAKE_MUTATE',None)
            args=['python3',str(ROOT/'scripts/qemu-test.py'),'x86_64','--userland','alpine','--memory','3072M',
                  '--disk',str(f.disk),'--cmdline','root=/dev/vda1','--boot-timeout','300','--timeout','780',
                  '--frozen-manifest',str(f.manifest),'--monitor-socket',f.monitor,'--qmp-socket',f.qmp,'--log',str(root/'run.log'),'echo harmless','host:touch '+str(root/'host-called')]
            if offline: args.insert(args.index('--frozen-manifest'), '--frozen-offline')
            result=subprocess.run(args,env=env,capture_output=True,text=True,timeout=30)
            self.assertTrue((root/'fake-called').exists(), result.stdout+result.stderr)
            self.assertFalse((root/'make-called').exists())
            self.assertEqual((root/'host-called').exists(), not root_fail)
            return result

    def test_offline_frozen_root_failure_skips_commands(self):
        r=self.run_runner(root_fail=True,offline=True)
        self.assertNotEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertIn('FROZEN_NETWORK_MODE=no-guest-nic',r.stdout)
        self.assertIn('FAIL   frozen root preflight',r.stdout)
        self.assertIn('SKIP   echo harmless',r.stdout)
        self.assertIn('SKIP   host:touch ',r.stdout)

    def test_offline_frozen_refuses_network_forwarding(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);f=Fixture(root);scripts=root/'scripts';scripts.mkdir()
            controller=(ROOT/'scripts/qemu-test.py').read_text()
            marker="ANSI = re.compile("
            self.assertEqual(controller.count(marker),1)
            interceptor="def forbidden_spawn(*args,**kwargs):\n    open("+repr(str(root/'spawn-called'))+",'w').close()\n    raise RuntimeError('unexpected spawn')\nsubprocess.Popen = forbidden_spawn\n\n"
            (scripts/'qemu-test.py').write_text(controller.replace(marker,interceptor+marker))
            (scripts/'net-host-server.py').write_text("from pathlib import Path\nPath("+repr(str(root/'server-called'))+").touch()\n")
            env=dict(os.environ,FAKE_CALLED=str(root/'fake-called'))
            env.pop('QEMU_EXTRA',None)
            args=['python3',str(scripts/'qemu-test.py'),'x86_64','--userland','alpine','--memory','3072M',
                  '--disk',str(f.disk),'--cmdline','root=/dev/vda1','--boot-timeout','300','--timeout','780',
                  '--frozen-manifest',str(f.manifest),'--frozen-offline','--net-test','--monitor-socket',f.monitor,
                  '--qmp-socket',f.qmp,'--log',str(root/'run.log'),'echo harmless']
            r=subprocess.run(args,env=env,capture_output=True,text=True,timeout=10)
            self.assertNotEqual(r.returncode,0)
            self.assertIn('FAIL   frozen preflight',r.stdout);self.assertIn('SKIP   echo harmless',r.stdout)
            self.assertFalse((root/'fake-called').exists())
            self.assertFalse((root/'server-called').exists())
            self.assertFalse((root/'spawn-called').exists())

    def test_offline_failed_host_or_guest_step_skips_rest(self):
        for guest_fail in (False,True):
            with self.subTest(guest_fail=guest_fail), tempfile.TemporaryDirectory() as td:
                root=Path(td); f=Fixture(root)
                env=dict(os.environ,FAKE_CALLED=str(root/'fake-called'))
                for key in ('QEMU_EXTRA','FAKE_MUTATE','FAKE_ROOT_FAIL','FAKE_RC_FAIL'): env.pop(key,None)
                if guest_fail: env['FAKE_RC_FAIL']='1'
                step='echo harmless' if guest_fail else 'host:false'
                later='host:touch '+str(root/'must-not-run')
                args=['python3',str(ROOT/'scripts/qemu-test.py'),'x86_64','--userland','alpine','--memory','3072M',
                      '--disk',str(f.disk),'--cmdline','root=/dev/vda1','--boot-timeout','300','--timeout','780',
                      '--frozen-manifest',str(f.manifest),'--frozen-offline','--monitor-socket',f.monitor,
                      '--qmp-socket',f.qmp,'--log',str(root/'run.log'),step,later]
                r=subprocess.run(args,env=env,capture_output=True,text=True,timeout=30)
                self.assertNotEqual(r.returncode,0,r.stdout+r.stderr)
                self.assertIn('FROZEN_NETWORK_MODE=no-guest-nic',r.stdout)
                self.assertIn('FAIL   '+step,r.stdout);self.assertIn('SKIP   '+later,r.stdout)
                self.assertFalse((root/'must-not-run').exists())

    def test_offline_detects_changed_golden(self):
        r=self.run_runner(mutate=True,offline=True)
        self.assertNotEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertIn('FAIL   frozen fixture verification: golden_disk changed',r.stdout)

    def test_frozen_runner_stops_after_failed_host_command(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);f=Fixture(root)
            env=dict(os.environ,FAKE_CALLED=str(root/'fake-called'),FAKE_POWER=str(root/'poweroff-sent'));env.pop('QEMU_EXTRA',None);env.pop('FAKE_MUTATE',None);env.pop('FAKE_ROOT_FAIL',None)
            args=['python3',str(ROOT/'scripts/qemu-test.py'),'x86_64','--userland','alpine','--memory','3072M',
                  '--disk',str(f.disk),'--cmdline','root=/dev/vda1','--boot-timeout','300','--timeout','780',
                  '--frozen-manifest',str(f.manifest),'--monitor-socket',f.monitor,'--qmp-socket',f.qmp,
                  '--log',str(root/'run.log'),'host:false','host:touch '+str(root/'must-not-run')]
            r=subprocess.run(args,env=env,capture_output=True,text=True,timeout=30)
            self.assertNotEqual(r.returncode,0,r.stdout+r.stderr)
            self.assertIn('FAIL   host:false',r.stdout);self.assertIn('SKIP   host:touch ',r.stdout)
            self.assertFalse((root/'must-not-run').exists())
            self.assertFalse((root/'poweroff-sent').exists())
            self.assertIn('terminating without further guest commands',r.stdout)

    def test_frozen_runner_stops_after_inert_guest_failure(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);f=Fixture(root)
            env=dict(os.environ,FAKE_CALLED=str(root/'fake-called'), FAKE_POWER=str(root/'poweroff-sent'), FAKE_RC_FAIL='1')
            for key in ('QEMU_EXTRA', 'FAKE_MUTATE', 'FAKE_ROOT_FAIL'): env.pop(key,None)
            later='host:touch '+str(root/'must-not-run')
            args=['python3',str(ROOT/'scripts/qemu-test.py'),'x86_64','--userland','alpine','--memory','3072M',
                  '--disk',str(f.disk),'--cmdline','root=/dev/vda1','--boot-timeout','300','--timeout','780',
                  '--frozen-manifest',str(f.manifest),'--monitor-socket',f.monitor,'--qmp-socket',f.qmp,
                  '--log',str(root/'run.log'),'echo harmless',later]
            r=subprocess.run(args,env=env,capture_output=True,text=True,timeout=30)
            self.assertNotEqual(r.returncode,0,r.stdout+r.stderr)
            self.assertIn('FAIL   echo harmless',r.stdout);self.assertIn('SKIP   '+later,r.stdout)
            self.assertFalse((root/'must-not-run').exists());self.assertFalse((root/'poweroff-sent').exists())

    def test_runner_rejects_bad_preflight_without_launch_or_make(self):
        for mode, offline in [(m,o) for m in ('wrong-hash', 'extra', 'missing-qmp', 'wrong-architecture', 'existing-log', 'missing-log', 'boot-timeout', 'step-timeout') for o in (False,True)]:
            with self.subTest(mode=mode,offline=offline), tempfile.TemporaryDirectory() as td:
                root=Path(td); f=Fixture(root); fakebin=root/'bin';fakebin.mkdir()
                maker=fakebin/'make';maker.write_text('#!/bin/sh\ntouch "$MAKE_CALLED"\nexit 77\n');maker.chmod(0o755)
                env=dict(os.environ,PATH=str(fakebin)+':/usr/bin:/bin',FAKE_CALLED=str(root/'fake-called'),MAKE_CALLED=str(root/'make-called'))
                env.pop('QEMU_EXTRA',None);env.pop('FAKE_MUTATE',None)
                if mode=='wrong-hash': f.data['iso']['sha256']='0'*64;f.save()
                if mode=='extra': env['QEMU_EXTRA']='-smp 8'
                args=['python3',str(ROOT/'scripts/qemu-test.py'), 'aarch64' if mode=='wrong-architecture' else 'x86_64',
                      '--userland','alpine','--memory','3072M','--disk',str(f.disk),'--cmdline','root=/dev/vda1',
                      '--boot-timeout','301' if mode=='boot-timeout' else '300',
                      '--timeout','781' if mode=='step-timeout' else '780',
                      '--frozen-manifest',str(f.manifest),'--monitor-socket',f.monitor]
                if offline: args.append('--frozen-offline')
                if mode!='missing-qmp': args.extend(['--qmp-socket',f.qmp])
                if mode=='existing-log': (root/'run.log').write_text('must not be truncated')
                if mode!='missing-log': args.extend(['--log',str(root/'run.log')])
                args.append('echo harmless')
                r=subprocess.run(args,env=env,capture_output=True,text=True,timeout=10)
                self.assertNotEqual(r.returncode,0,r.stdout+r.stderr)
                self.assertIn('FAIL   frozen preflight',r.stdout)
                self.assertIn('SKIP   echo harmless',r.stdout)
                self.assertFalse((root/'fake-called').exists(),r.stdout+r.stderr)
                self.assertFalse((root/'make-called').exists(),r.stdout+r.stderr)
                if mode=='existing-log':self.assertEqual((root/'run.log').read_text(),'must not be truncated')

    def test_startup_failure_marks_every_step_skipped_and_keeps_log(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);f=Fixture(root)
            # A regular executable with a missing interpreter passes file/hash
            # preflight but cannot execute. It never starts a guest or stub.
            f.qemu.write_text('#!/definitely-missing-m40-interpreter\n');f.qemu.chmod(0o755)
            f.data['qemu']['sha256']=hashlib.sha256(f.qemu.read_bytes()).hexdigest();f.save()
            log=root/'run.log'
            args=['python3',str(ROOT/'scripts/qemu-test.py'),'x86_64','--userland','alpine','--memory','3072M',
                  '--disk',str(f.disk),'--cmdline','root=/dev/vda1','--boot-timeout','300','--timeout','780',
                  '--frozen-manifest',str(f.manifest),'--monitor-socket',f.monitor,'--qmp-socket',f.qmp,'--log',str(log),
                  'echo harmless','host:touch '+str(root/'host-called')]
            env=dict(os.environ);env.pop('QEMU_EXTRA',None)
            r=subprocess.run(args,env=env,capture_output=True,text=True,timeout=10)
            self.assertNotEqual(r.returncode,0,r.stdout+r.stderr)
            for phrase in ('FAIL   launcher startup','SKIP   echo harmless','SKIP   host:touch '):
                self.assertIn(phrase,r.stdout);self.assertIn(phrase,log.read_text())
            self.assertFalse((root/'host-called').exists())
            self.assertEqual(f.disk.read_bytes(),b'INERT DISK')

    def test_host_helper_timeout_kills_group_and_keeps_partial_output(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);child=root/'child.py';pidfile=root/'pid'
            child.write_text("import os,time\nopen("+repr(str(pidfile))+",'w').write(str(os.getpid()))\nprint('PARTIAL CONTROL',flush=True)\ntime.sleep(60)\n")
            out,passed=FROZEN.run_host('python3 '+str(child),.8)
            self.assertFalse(passed);self.assertIn(b'PARTIAL CONTROL',out);self.assertIn(b'(timeout)',out)
            pid=int(pidfile.read_text())
            # On container systems an adopted dead child may briefly be a zombie.
            proc=Path('/proc')/str(pid)/'stat'
            deadline=time.monotonic()+3
            while proc.exists() and proc.read_text().split()[2] != 'Z' and time.monotonic()<deadline:time.sleep(.05)
            self.assertTrue(not proc.exists() or proc.read_text().split()[2]=='Z','timed-out child still executing')

    def test_escaped_descendant_pipe_timeout_returns_failure(self):
        # This test explicitly kills the intentionally escaped inert child.
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); child=root/'escaped.py'; pidfile=root/'pid'
            child.write_text("import subprocess,sys,time\ncode = " + repr("import os,time\npath="+repr(str(pidfile))+"\nopen(path+'.tmp','w').write(str(os.getpid()))\nos.replace(path+'.tmp',path)\nprint('ESCAPED PARTIAL',flush=True)\ntime.sleep(1.5)\nprint('ESCAPED LATE',flush=True)\ntime.sleep(60)\n") + "\nsubprocess.Popen([sys.executable, '-c', code], start_new_session=True)\ntime.sleep(60)\n")
            start=time.monotonic()
            def watchdog(signum, frame): raise TimeoutError('host control watchdog')
            previous=signal.signal(signal.SIGALRM, watchdog)
            signal.alarm(16)
            try:
                out,passed=FROZEN.run_host('python3 '+str(child), .8)
                self.assertFalse(passed)
                self.assertEqual(out.count(b'ESCAPED PARTIAL'),1)
                self.assertEqual(out.count(b'ESCAPED LATE'),1)
                self.assertIn(b'timeout; output pipe still open, incomplete',out)
                self.assertLess(time.monotonic()-start,15)
            finally:
                signal.alarm(0); signal.signal(signal.SIGALRM, previous)
                if pidfile.exists():
                    try: os.kill(int(pidfile.read_text()), signal.SIGKILL)
                    except ProcessLookupError: pass

    def test_escaped_pipe_failure_stops_inert_controller(self):
        # Copy the real controller and launcher; only shorten the helper timeout
        # inside this disposable test copy, never the production deadline.
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); f=Fixture(root); scripts=root/'scripts'; scripts.mkdir()
            shutil.copy(ROOT/'scripts/qemu-test.py', scripts/'qemu-test.py')
            original=(ROOT/'scripts/qemu-frozen.py').read_text()
            (scripts/'qemu-frozen.py').write_text(original + "\n_real_run_host = run_host\ndef run_host(command, timeout): return _real_run_host(command, .8)\n")
            child=root/'escaped.py'; pidfile=root/'pid'
            child.write_text("import subprocess,sys,time\ncode = " + repr("import os,time\npath="+repr(str(pidfile))+"\nopen(path+'.tmp','w').write(str(os.getpid()))\nos.replace(path+'.tmp',path)\nprint('ESCAPED PARTIAL',flush=True)\ntime.sleep(1.5)\nprint('ESCAPED LATE',flush=True)\ntime.sleep(60)\n") + "\nsubprocess.Popen([sys.executable, '-c', code], start_new_session=True)\ntime.sleep(60)\n")
            later='host:touch '+str(root/'must-not-run')
            args=['python3',str(scripts/'qemu-test.py'),'x86_64','--userland','alpine','--memory','3072M',
                  '--disk',str(f.disk),'--cmdline','root=/dev/vda1','--boot-timeout','300','--timeout','780',
                  '--frozen-manifest',str(f.manifest),'--monitor-socket',f.monitor,'--qmp-socket',f.qmp,
                  '--log',str(root/'run.log'),'host:python3 '+str(child),later]
            env=dict(os.environ, FAKE_CALLED=str(root/'fake-called'))
            for key in ('QEMU_EXTRA','FAKE_MUTATE','FAKE_ROOT_FAIL','FAKE_RC_FAIL'): env.pop(key,None)
            try:
                r=subprocess.run(args,env=env,capture_output=True,text=True,timeout=25)
                self.assertNotEqual(r.returncode,0,r.stdout+r.stderr)
                self.assertEqual(r.stdout.count('ESCAPED PARTIAL'),1)
                self.assertEqual(r.stdout.count('ESCAPED LATE'),1)
                self.assertIn('timeout; output pipe still open, incomplete',r.stdout)
                self.assertIn('FAIL   host:python3 ',r.stdout);self.assertIn('SKIP   '+later,r.stdout)
                self.assertFalse((root/'must-not-run').exists())
                log=(root/'run.log').read_text()
                self.assertEqual(log.count('ESCAPED PARTIAL'),1)
                self.assertIn('timeout; output pipe still open, incomplete',log)
            finally:
                if pidfile.exists():
                    try: os.kill(int(pidfile.read_text()), signal.SIGKILL)
                    except ProcessLookupError: pass

    def test_cleanup_errors_cannot_hide_timeout(self):
        pipe=mock.Mock()
        proc=mock.Mock(pid=12345, stdout=pipe)
        proc.communicate.side_effect=[subprocess.TimeoutExpired('helper',1,output=b'part'),
                                      subprocess.TimeoutExpired('helper',10,output=b'partial')]
        proc.poll.return_value=None
        proc.wait.side_effect=subprocess.TimeoutExpired('helper',10)
        with mock.patch.object(FROZEN.subprocess,'Popen',return_value=proc), \
             mock.patch.object(FROZEN.os,'killpg',side_effect=[PermissionError('denied'),ProcessLookupError()]):
            out,passed=FROZEN.run_host('inert',1)
        self.assertFalse(passed)
        self.assertEqual(out.count(b'partial'),1)
        self.assertIn(b'timeout; output pipe still open, incomplete',out)
        self.assertIn(b'initial group kill',out);self.assertIn(b'helper reap',out)
        pipe.close.assert_called_once()

    def test_pipe_close_error_preserves_timeout_output(self):
        pipe=mock.Mock(); pipe.close.side_effect=OSError('close denied')
        proc=mock.Mock(pid=12345,stdout=pipe)
        proc.communicate.side_effect=[subprocess.TimeoutExpired('helper',1,output=b'part'),
                                      subprocess.TimeoutExpired('helper',10,output=b'partial')]
        proc.poll.return_value=0
        with mock.patch.object(FROZEN.subprocess,'Popen',return_value=proc), \
             mock.patch.object(FROZEN.os,'killpg'):
            out,passed=FROZEN.run_host('inert',1)
        self.assertFalse(passed);self.assertIn(b'partial',out)
        self.assertIn(b'timeout; output pipe still open, incomplete',out)
        self.assertIn(b'output pipe close',out)

    def test_host_helper_status_is_not_hidden(self):
        out,passed=FROZEN.run_host("printf success",2);self.assertTrue(passed);self.assertEqual(out,b'success')
        out,passed=FROZEN.run_host("printf failure; exit 7",2);self.assertFalse(passed);self.assertEqual(out,b'failure')

    def test_interrupt_reaps_inert_process_group(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);f=Fixture(root)
            env=dict(os.environ, FAKE_CALLED=str(root/'fake-called'))
            env.pop('QEMU_EXTRA',None);env.pop('FAKE_MUTATE',None)
            args=['python3',str(ROOT/'scripts/qemu-test.py'),'x86_64','--userland','alpine','--memory','3072M',
                  '--disk',str(f.disk),'--cmdline','root=/dev/vda1','--boot-timeout','300','--timeout','780',
                  '--frozen-manifest',str(f.manifest),'--monitor-socket',f.monitor,'--qmp-socket',f.qmp,
                  '--log',str(root/'run.log'),'echo harmless']
            p=subprocess.Popen(args,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            try:
                deadline=time.monotonic()+10
                while not (root/'fake-called').exists() and p.poll() is None and time.monotonic()<deadline:time.sleep(.05)
                self.assertTrue((root/'fake-called').exists(),'inert stub did not launch')
                pid=int((root/'fake-called').read_text());p.send_signal(signal.SIGINT)
                out,err=p.communicate(timeout=15)
                self.assertNotEqual(p.returncode,0,(out+err).decode(errors='replace'))
                with self.assertRaises(ProcessLookupError):os.kill(pid,0)
            finally:
                if p.poll() is None:p.kill();p.wait()

    def test_runner_uses_only_inert_frozen_executable(self):
        r=self.run_runner()
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertIn('PASS   echo harmless',r.stdout)
        self.assertIn('FROZEN_NETWORK_MODE=user-network',r.stdout)

    def test_failed_root_gate_skips_all_requested_steps(self):
        r=self.run_runner(root_fail=True)
        self.assertNotEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertIn('FAIL   frozen root preflight',r.stdout)
        self.assertIn('SKIP   echo harmless',r.stdout)
        self.assertIn('SKIP   host:touch ',r.stdout)
        self.assertNotIn('PASS   echo harmless',r.stdout)

    def test_real_root_gate_expression(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); mounts=root/'mounts'; dbus=root/'dbus'; pam=root/'pam'; shadow=root/'shadow'; shadow.touch()
            fakebin=root/'bin'; fakebin.mkdir(); fake_stat=fakebin/'stat'
            fake_stat.write_text('#!/bin/sh\ncase "$3" in */dbus) echo "${DBUS_META}";; */pam) echo "${PAM_META}";; */shadow) echo "${SHADOW_META}";; *) exit 1;; esac\n')
            fake_stat.chmod(0o755)
            env=dict(os.environ,PATH=str(fakebin)+':/usr/bin:/bin',DBUS_META='0:81:4750',PAM_META='0:42:2755',SHADOW_META='0:42:640')
            command=FROZEN.root_gate_command(str(mounts),str(dbus),str(pam),str(shadow))
            def check(rows, dbus_meta='0:81:4750', pam_meta='0:42:2755', shadow_meta='0:42:640'):
                mounts.write_text(rows)
                e=dict(env,DBUS_META=dbus_meta,PAM_META=pam_meta,SHADOW_META=shadow_meta)
                return subprocess.run(['/bin/sh','-c',command],env=e).returncode
            good='/dev/vda1 / ext2 rw 0 0\nproc /proc proc rw 0 0\n'
            self.assertEqual(check(good),0)
            for rows in ('initramfs / tmpfs rw 0 0\n', '/dev/vda1 / tmpfs rw 0 0\n',
                         '/dev/vda2 / ext2 rw 0 0\n', good + '/dev/vda1 / ext2 rw 0 0\n', ''):
                self.assertNotEqual(check(rows),0,rows)
            self.assertNotEqual(check(good,dbus_meta='0:0:4750'),0)
            self.assertNotEqual(check(good,pam_meta='0:0:2755'),0)
            self.assertNotEqual(check(good,pam_meta='0:42:755'),0)
            for bad in ('0:0:640','0:42:644','1:42:640'):
                self.assertNotEqual(check(good,shadow_meta=bad),0)
            shadow.unlink();self.assertNotEqual(check(good),0)
            shadow.symlink_to('/etc/passwd');self.assertNotEqual(check(good),0)

    def test_runner_marks_changed_golden_as_failure(self):
        r=self.run_runner(mutate=True)
        self.assertNotEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertIn('FAIL   frozen fixture verification: golden_disk changed',r.stdout)


class FrozenKmsWrapper(unittest.TestCase):
    def run_wrapper(self, manifest=True, disk=True, diagnostic=True, extra='', option=None, ocr=True, unsafe_workspace=False):
        with tempfile.TemporaryDirectory(prefix='frozen-kms-inert-') as td:
            root=Path(td)
            if unsafe_workspace: root=root/unsafe_workspace;root.mkdir()
            scripts=root/'scripts';scripts.mkdir();(root/'build').mkdir()
            shutil.copy2(ROOT/'scripts/alpine-kms-tests.sh', scripts/'alpine-kms-tests.sh')
            fake_runner=scripts/'qemu-test.py'
            fake_runner.write_text("import json,os,sys\nfrom pathlib import Path\na=sys.argv[1:]\nPath(os.environ['CAPTURE']).write_text(json.dumps(a))\np=Path(a[a.index('--log')+1]);p.write_text('')\n")
            (scripts/'input-chain-check.py').write_text("print('INERT CHECK ONLY')\n")
            mkdisk=scripts/'mkdisk.sh';mkdisk.write_text('#!/bin/sh\ntouch "$DISK_CALLED"\nexit 9\n');mkdisk.chmod(0o755)
            fixture=root/'userland/alpine-desktop-root-x86_64/usr/bin';fixture.mkdir(parents=True)
            for name in ('desktopframecheck','desktopa11y','desktopa11y-dbus','input-capture-session',
                         'inputeventcapture','input-capture-ready','konsole-key-trace','wayland-presentation-proxy'):
                f=fixture/name;f.write_text('inert');f.chmod(0o755)
            lib=fixture.parent/'lib';lib.mkdir();(lib/'lib9os-qt-key-observer.so').write_text('inert')
            bindir=root/'bin';bindir.mkdir();tesseract=bindir/'tesseract'
            tesseract.write_text('#!/bin/sh\n'+('echo eng; exit 1\n' if ocr == 'bad-status' else 'echo eng\n' if ocr else 'exit 1\n'));tesseract.chmod(0o755)
            env=dict(os.environ,PATH=str(bindir)+':/usr/bin:/bin',CAPTURE=str(root/'capture.json'),
                     DISK_CALLED=str(root/'disk-called'),QEMU_EXTRA=extra,
                     ALPINE_INPUT_CHAIN_DIAGNOSTICS='1' if diagnostic else '0',
                     ALPINE_QT_KEY_TRACE='0',ALPINE_A11Y_DBUS_VALIDATE='0',ALPINE_TERMINAL_QUERY='atspi')
            env.pop('M40_FROZEN_MANIFEST',None);env.pop('M40_FROZEN_DISK',None);env.pop('M40_FROZEN_OFFLINE',None)
            if option:env.update(option)
            if manifest:env['M40_FROZEN_MANIFEST']=str(root/'manifest.json')
            if disk:env['M40_FROZEN_DISK']=str(root/'disposable.raw')
            r=subprocess.run(['/bin/sh',str(scripts/'alpine-kms-tests.sh'),'x86_64'],cwd=root,env=env,
                             capture_output=True,text=True,timeout=10)
            called=(root/'capture.json').exists()
            args=json.loads((root/'capture.json').read_text()) if called else []
            return r, args, (root/'disk-called').exists(), called

    def test_frozen_wrapper_passes_fixed_args_without_mkdisk(self):
        r,args,disk_called,runner_called=self.run_wrapper()
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertTrue(runner_called);self.assertFalse(disk_called)
        self.assertNotIn('--frozen-offline',args)
        for flag in ('--frozen-manifest','--monitor-socket','--qmp-socket'):
            self.assertEqual(args.count(flag),1)
        self.assertIn('--require-input-capture-ready',next(a for a in args if a.startswith('host:python3 scripts/qemu-ready-type.py')))
        self.assertEqual(args[args.index('--cmdline')+1],'root=/dev/vda1')
        self.assertIn('ORCHESTRATED_CONTROLLER_RC=0',r.stdout)
        self.assertIn('INPUT_CHAIN_CHECK_RC=0',r.stdout)
        self.assertIn('FROZEN_OUTPUT_DIR=',r.stdout)

    def test_zero_offline_option_retains_existing_default(self):
        r,args,disk_called,runner_called=self.run_wrapper(option={'M40_FROZEN_OFFLINE':'0'})
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertTrue(runner_called);self.assertFalse(disk_called)
        self.assertNotIn('--frozen-offline',args)

    def test_offline_wrapper_explicitly_selects_no_nic(self):
        r,args,disk_called,runner_called=self.run_wrapper(option={'M40_FROZEN_OFFLINE':'1'})
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertTrue(runner_called);self.assertFalse(disk_called)
        self.assertEqual(args.count('--frozen-offline'),1)
        self.assertEqual(args[args.index('--boot-timeout')+1],'300')
        self.assertEqual(args[args.index('--timeout')+1],'780')

    def test_frozen_rejects_each_unsafe_workspace_path(self):
        for path in ('space name','semi;colon','trailing-newline\n'):
            with self.subTest(path=repr(path)):
                r,args,disk_called,runner_called=self.run_wrapper(unsafe_workspace=path, option={'M40_FROZEN_OFFLINE':'1'})
                self.assertNotEqual(r.returncode,0)
                self.assertIn('Frozen mode requires a shell-safe workspace path',r.stderr)
                self.assertFalse(disk_called);self.assertFalse(runner_called)

    def test_incomplete_or_overridden_configuration_fails_before_disk_and_runner(self):
        for kwargs in ({'manifest':False},{'disk':False},{'diagnostic':False},{'extra':'-smp 8'},
                       {'manifest':False,'disk':False,'option':{'M40_FROZEN_OFFLINE':'1'}},
                       {'manifest':False,'option':{'M40_FROZEN_OFFLINE':'1'}},
                       {'disk':False,'option':{'M40_FROZEN_OFFLINE':'1'}},
                       {'option':{'M40_FROZEN_OFFLINE':'invalid'}},{'option':{'M40_FROZEN_OFFLINE':''}},
                       {'ocr':False},{'ocr':'bad-status'},{'option':{'ALPINE_QT_KEY_TRACE':'1'}},
                       {'option':{'ALPINE_A11Y_DBUS_VALIDATE':'1'}},
                       {'option':{'ALPINE_TERMINAL_QUERY':'live-dbus'}}):
            with self.subTest(kwargs=kwargs):
                r,args,disk_called,runner_called=self.run_wrapper(**kwargs)
                self.assertNotEqual(r.returncode,0)
                self.assertFalse(disk_called);self.assertFalse(runner_called)


if __name__ == '__main__':unittest.main()
