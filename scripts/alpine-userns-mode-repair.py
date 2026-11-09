#!/usr/bin/env python3
"""Opt-in x86 signed-mode restoration in a single-ID user namespace.

This repairs ONLY the explicitly pinned files below. It is not whole-image
verification, guest evidence, authorization to boot, or an initramfs fix.
Never execute staged files on the host. Build and package in a private namespace.
"""
import argparse, hashlib, importlib.util, json, os, pathlib, stat, sys, tarfile, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
LOCK_SHA = {'base':'88614612f69e42ab6dd8fab32f7e376ff8ce0386855e8580c0bbfdd895f6df86',
            'desktop':'c3665b496c3f3657a3290ee8d613c4513715827cf0d6c7e572debe21aa829ede'}
# apk, apk sha256, path, signed group, mode, payload sha256, desktop-only
FILES = [
 ('busybox-suid-1.37.0-r20.apk','f139a0f28d40a3a183661ee1ad7a248fcf01e82b144114f86d3bc16cee990917','bin/bbsuid','root',0o4111,'a5b65fb3dee405be3c7a6f8d219ab9a432bda4813cf6a8b98279c2a12a454533',False),
 ('dbus-daemon-launch-helper-1.16.2-r1.apk','8845aa8f7eb24b39cc91b9053bbb81c762f512a5ee4320a13f0c2e2bf06f4267','usr/libexec/dbus-daemon-launch-helper','messagebus',0o4750,'830a922580016da9dbbdeed5fef2e010480322caed6bbf3f71454e28c4409ad0',False),
 ('linux-pam-1.7.0-r4.apk','0152e9cd1d13986b75e4ccec8155a878791083ddca7e2667bbbb398bc62c9703','usr/sbin/unix_chkpwd','shadow',0o2755,'945e0d69098998e09472de552597034278c7ab5a666f6d6f6fa15027f11dab09',True),
 ('fuse3-3.16.2-r1.apk','c38bb18c948ac92e0b27725d5a1a274e6f73fed777531d70771c417f6240bdf3','usr/bin/fusermount3','root',0o4755,'88f00af67c63cb8a474cea13599c1c4544fe60f5e2113e65de2928395bf0f41b',True),
 *[('shadow-4.17.3-r0.apk','498c24794f42cac2bcd44240e52f986aeaa43b9176e5d9532faf0e1707826692','usr/bin/'+n,'root',0o4755,h,True) for n,h in [
 ('chage','056dbf83740d5754d59dae50f99481b0e17a9eff67e4b955aaa331d2bdaed127'),('chfn','fe50c0a2b750346b0147d44f1adeca689fb3f5b97eda2701d64ab48d476b18cf'),
 ('chsh','6e9e5cb9e7a2f4c6c92211d6e7fd13fc99e04c74d53afbf147fb3e4f057f502f'),('expiry','12739ed099c72596b767b4e349a8db24cb70a073ab18d136878951bb7b9cbe98'),
 ('gpasswd','012ccbc78fa71a14521b16cd8cd748e47b15b2f04925fde25df3f3b6008f72cd'),('passwd','c7abfbcd3ab6864bf22c7a327dafeccc0cb16642a2b59f2bdaf96cea75237c05')]]
]

def require(ok, reason):
 if not ok: raise ValueError(reason)

def sha(path):
 with open(path,'rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def namespace_check():
 for name in ('uid_map','gid_map'):
  row=pathlib.Path('/proc/self/'+name).read_text().split()
  require(len(row)==3 and row[0]=='0' and row[1]!='0' and row[2]=='1','requires single-ID non-root '+name)
 require(os.getuid()==0 and os.getgid()==0,'requires namespace uid/gid 0')

def open_beneath(rootfd, name):
 parts=name.split('/');require(all(x not in ('','.','..') for x in parts),'bad path')
 fd=os.dup(rootfd)
 try:
  for part in parts[:-1]:
   nxt=os.open(part,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=fd);os.close(fd);fd=nxt
  return os.open(parts[-1],os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=fd)
 finally:os.close(fd)

def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('stage',type=pathlib.Path);ap.add_argument('--profile',choices=LOCK_SHA,required=True);ap.add_argument('--cache',type=pathlib.Path,required=True);args=ap.parse_args()
 namespace_check();stage=args.stage;cache=args.cache.resolve()
 lock=ROOT/'userland/alpine/locks'/('x86_64.json' if args.profile=='base' else 'x86_64-desktop.json')
 require(sha(lock)==LOCK_SHA[args.profile],'unreviewed lock')
 entries=json.loads(lock.read_text())['packages']; files=[f for f in FILES if args.profile=='desktop' or not f[-1]]
 packages={}
 for apk, apksha, *_ in files:
  matches=[e for e in entries if e['url'].endswith('/'+apk) and e['sha256']==apksha]
  require(len(matches)==1,'package not uniquely pinned: '+apk)
  path=cache/apk;require(path.is_file() and not path.is_symlink() and sha(path)==apksha,'package hash mismatch: '+apk);packages[apk]=path
 spec=importlib.util.spec_from_file_location('oracle_packages',ROOT/'scripts/evdev-linux-oracle-packages.py');oracle=importlib.util.module_from_spec(spec);spec.loader.exec_module(oracle)
 with tempfile.TemporaryDirectory(prefix='m40-mode-verifier-') as tmp:
  verifier=oracle.bootstrap(oracle.load_lock(oracle.LOCK),cache,pathlib.Path(tmp),True,oracle.checked)
  oracle.checked(verifier+['verify']+[str(x) for x in packages.values()])
 for apk, _, name, group, mode, expected, _ in files:
  with tarfile.open(packages[apk],'r:gz',ignore_zeros=True) as t:
   members=[m for m in t if m.name==name]
   require(len(members)==1 and members[0].isfile() and 0<members[0].size<=1024*1024,'unsafe signed member: '+name)
   m=members[0];require((m.uname,m.gname,m.mode)==('root',group,mode),'signed metadata mismatch: '+name)
   require(hashlib.sha256(t.extractfile(m).read()).hexdigest()==expected,'signed payload mismatch: '+name)
 rootfd=os.open(stage,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC); opened=[]
 try:
  for _,_,name,_,mode,expected,_ in files:
   fd=open_beneath(rootfd,name);opened.append((fd,name,mode,expected));s=os.fstat(fd)
   require(stat.S_ISREG(s.st_mode) and s.st_nlink==1 and s.st_uid==0 and s.st_gid==0,'unexpected inode/owner/links: '+name)
   require(stat.S_IMODE(s.st_mode) in (mode,mode & ~0o6000),'unexpected mode: '+name)
   with os.fdopen(os.dup(fd),'rb') as f:actual=hashlib.file_digest(f,'sha256').hexdigest()
   require(actual==expected,'stage payload differs from signed package: '+name)
  # A failure leaves only the private temporary stage; caller must discard it.
  for fd,name,mode,expected in opened:os.fchmod(fd,mode)
  for fd,name,mode,expected in opened:
   require(stat.S_IMODE(os.fstat(fd).st_mode)==mode,'post-write mode mismatch: '+name)
   os.lseek(fd,0,os.SEEK_SET)
   with os.fdopen(os.dup(fd),'rb') as f:require(hashlib.file_digest(f,'sha256').hexdigest()==expected,'post-write payload mismatch: '+name)
   print('VERIFIED',name,oct(mode),expected)
 finally:
  for fd,_,_,_ in opened:os.close(fd)
  os.close(rootfd)
 print('Only pinned x86 file modes restored; ownership, xattrs, initramfs and guest security remain separate gates.')

if __name__=='__main__':
 try:main()
 except (OSError, ValueError, tarfile.TarError) as e:sys.exit('mode repair refused: '+str(e))
