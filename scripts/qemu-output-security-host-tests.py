#!/usr/bin/env python3
"""Inert host-only tests for screenshot and frozen wrapper output safety."""
import os,pathlib,shutil,socket,subprocess,sys,tempfile,threading,unittest
from PIL import Image
ROOT=pathlib.Path(__file__).resolve().parents[1]
class OutputSafety(unittest.TestCase):
 def test_screenshot_refuses_existing_file_and_symlink(self):
  with tempfile.TemporaryDirectory() as d:
   d=pathlib.Path(d); victim=d/'victim';victim.write_text('untouched')
   for kind in ('file','symlink'):
    p=d/(kind+'.png')
    if kind=='file':p.write_text('existing')
    else:p.symlink_to(victim)
    r=subprocess.run([sys.executable,str(ROOT/'scripts/qemu-screenshot.py'),str(d/'absent.sock'),str(p)],capture_output=True,text=True,timeout=5)
    self.assertNotEqual(r.returncode,0);self.assertIn('must be fresh',r.stderr)
    self.assertEqual(victim.read_text(),'untouched')
    if kind=='file':self.assertEqual(p.read_text(),'existing')
 def test_screenshot_accepts_only_fresh_output_from_inert_monitor(self):
  with tempfile.TemporaryDirectory() as d:
   d=pathlib.Path(d);sock=d/'monitor.sock';ready=threading.Event();errors=[]
   def serve():
    try:
     with socket.socket(socket.AF_UNIX) as s:
      s.bind(str(sock));s.listen(1);ready.set()
      with s.accept()[0] as c:
       c.sendall(b'(qemu) ');cmd=c.recv(8192).decode().strip();self.assertTrue(cmd.startswith('screendump '))
       path=pathlib.Path(cmd[len('screendump '):]);im=Image.new('RGB',(640,480));im.putdata([((i>>8)&255,i&255,(i>>16)&255) for i in range(640*480)]);im.save(path,format='PPM');c.sendall(b'(qemu) ')
    except BaseException as e:errors.append(e);ready.set()
   t=threading.Thread(target=serve,daemon=True);t.start();self.assertTrue(ready.wait(3));p=d/'screen.png';legacy=d/'screen.ppm';legacy.write_text('untouched')
   r=subprocess.run([sys.executable,str(ROOT/'scripts/qemu-screenshot.py'),str(sock),str(p)],capture_output=True,text=True,timeout=10);t.join(3)
   if errors:raise errors[0]
   self.assertEqual(r.returncode,0,r.stderr)
   with Image.open(p) as image:self.assertEqual(image.size,(640,480))
   self.assertEqual(legacy.read_text(),'untouched')
 def test_dolphin_fresh_outputs_and_collision(self):
  with tempfile.TemporaryDirectory() as d:
   d=pathlib.Path(d);scripts=d/'scripts';scripts.mkdir();shutil.copy2(ROOT/'scripts/qemu-dolphin-capture.py',scripts/'qemu-dolphin-capture.py')
   (scripts/'qemu-screenshot.py').write_text('import pathlib,sys\npathlib.Path(sys.argv[2]).write_text("inert screenshot")\n')
   (scripts/'desktop-visible-check.py').write_text('import pathlib,sys\npathlib.Path(sys.argv[3]).write_text("inert OCR")\nprint("inert visible")\n')
   dest=d/'output.png';cmd=[sys.executable,str(scripts/'qemu-dolphin-capture.py'),'unused',str(dest)]
   r=subprocess.run(cmd,capture_output=True,text=True,timeout=10);self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(dest.read_text(),'inert screenshot');self.assertEqual(pathlib.Path(str(dest)+'.ocr.txt').read_text(),'inert OCR')
   r=subprocess.run(cmd,capture_output=True,text=True,timeout=5);self.assertNotEqual(r.returncode,0);self.assertEqual(dest.read_text(),'inert screenshot')
   dest.unlink();pathlib.Path(str(dest)+'.ocr.txt').unlink();victim=d/'victim';victim.write_text('untouched');dest.symlink_to(victim)
   r=subprocess.run(cmd,capture_output=True,text=True,timeout=5);self.assertNotEqual(r.returncode,0);self.assertEqual(victim.read_text(),'untouched')
 def test_frozen_wrapper_does_not_append_to_symlink_log(self):
  with tempfile.TemporaryDirectory() as td:
   root=pathlib.Path(td);scripts=root/'scripts';scripts.mkdir();(root/'build').mkdir();shutil.copy2(ROOT/'scripts/alpine-kms-tests.sh',scripts/'alpine-kms-tests.sh')
   victim=root/'victim';victim.write_text('untouched')
   (scripts/'qemu-test.py').write_text('import os,pathlib,sys\na=sys.argv\np=pathlib.Path(a[a.index("--log")+1]);p.symlink_to(os.environ["VICTIM"]);raise SystemExit(17)\n')
   (scripts/'input-chain-check.py').write_text('import os,pathlib\npathlib.Path(os.environ["CHECK_CALLED"]).touch()\n')
   (scripts/'mkdisk.sh').write_text('#!/bin/sh\nexit 99\n');(scripts/'mkdisk.sh').chmod(0o755)
   bindir=root/'bin';bindir.mkdir();(bindir/'tesseract').write_text('#!/bin/sh\necho eng\n');(bindir/'tesseract').chmod(0o755)
   f=root/'userland/alpine-desktop-root-x86_64/usr/bin';f.mkdir(parents=True)
   for name in ('desktopframecheck','desktopa11y','desktopa11y-dbus','input-capture-session','inputeventcapture','input-capture-ready','konsole-key-trace','wayland-presentation-proxy'):
    p=f/name;p.write_text('inert');p.chmod(0o755)
   lib=f.parent/'lib';lib.mkdir();(lib/'lib9os-qt-key-observer.so').write_text('inert')
   env=dict(os.environ,PATH=str(bindir)+':/usr/bin:/bin',M40_FROZEN_MANIFEST=str(root/'manifest'),M40_FROZEN_DISK=str(root/'disk'),ALPINE_INPUT_CHAIN_DIAGNOSTICS='1',ALPINE_QT_KEY_TRACE='0',ALPINE_A11Y_DBUS_VALIDATE='0',ALPINE_TERMINAL_QUERY='atspi',QEMU_EXTRA='',VICTIM=str(victim),CHECK_CALLED=str(root/'check-called'))
   r=subprocess.run(['/bin/sh',str(scripts/'alpine-kms-tests.sh'),'x86_64'],cwd=root,env=env,capture_output=True,text=True,timeout=10)
   self.assertEqual(r.returncode,17,r.stdout+r.stderr);self.assertEqual(victim.read_text(),'untouched');self.assertFalse((root/'check-called').exists());self.assertIn('SKIP input-chain checker',r.stdout);self.assertIn('ORCHESTRATED_CONTROLLER_RC=17',r.stdout);self.assertIn('INPUT_CHAIN_CHECK_RC=1',r.stdout)
if __name__=='__main__':unittest.main()
