#!/usr/bin/env python3
"""Prepare a fixed x86 Linux/9os experiment. No desktop library compilation."""
from pathlib import Path
import gzip,hashlib,json,os,shutil,subprocess
TOP=Path(__file__).resolve().parents[1];D=TOP/'build/m40-cold';S=TOP/'userland/alpine-root-x86_64';R=TOP/'userland/alpine-desktop-root-x86_64'
os.chdir(TOP)
assert not D.exists(), 'refuse to overwrite a prior plan'
D.mkdir(parents=True)
# Record a NEW predeclared host constraint, not a retry or target runtime fix.
cpus=sorted(os.sched_getaffinity(0))
assert len(cpus)>=2, 'two logical host CPUs required; no fallback'
profile={'available_cpus':cpus,'selected_cpus':cpus[:2],
 'scope':'two logical host CPUs; guest SMP4/3072M unchanged; no host memory cap',
 'meminfo':Path('/proc/meminfo').read_text(),
 'cpu_max':Path('/sys/fs/cgroup/cpu.max').read_text() if Path('/sys/fs/cgroup/cpu.max').exists() else 'unavailable'}
(D/'host-profile.json').write_text(json.dumps(profile,indent=2)+'\n')
assert not subprocess.check_output(['git','diff','935bd746cb6ea6d9ed833a5125b8f4465dc31ee7','--','kernel','userland','scripts/qemu-test.py','scripts/qemu-ready-type.py']), 'runtime code must match PR35 exactly'
# The production desktop suite does not install this extra raw-input observer.
# Copy only the same-source tiny consumer, before any fixture is frozen.
shutil.copy2(S/'usr/bin/inputeventcapture',R/'usr/bin/inputeventcapture')
K=D/'linux';K.mkdir()
with (D/'linux-packages.out').open('w')as log:
 subprocess.run([str(S/'lib/ld-musl-x86_64.so.1'),'--library-path',str(S/'lib')+':'+str(S/'usr/lib'),str(S/'sbin/apk'),'--arch','x86_64','--root',str(K),'--initdb','--no-scripts','--no-chown','--no-cache','--keys-dir',str(S/'etc/apk/keys'),'--repository','https://dl-cdn.alpinelinux.org/alpine/v3.22/main','add','linux-virt=6.12.113-r0'],stdout=log,stderr=subprocess.STDOUT,check=True)
I=D/'linux-init'
for n in ['bin','lib','dev','proc','sys','tmp','rootfs','modules']:(I/n).mkdir(parents=True,exist_ok=True)
shutil.copy2(R/'bin/busybox',I/'bin/busybox');shutil.copy2((R/'lib/ld-musl-x86_64.so.1').resolve(),I/'lib/ld-musl-x86_64.so.1')
for n in ['sh','mount','insmod','poweroff','sleep','mkdir','chmod','chown','ln','rm','chroot']:(I/'bin'/n).symlink_to('busybox')
M=K/'lib/modules/6.12.113-0-virt';deps={a:b.split()for a,b in (l.split(':')for l in (M/'modules.dep').read_text().splitlines())};order=[]
def add(p):
 if p in order:return
 for dep in deps[p]:add(dep)
 order.append(p)
for n in ['virtio_blk','ext2','evdev','virtio_input','bochs']:add(next(p for p in deps if p.endswith('/'+n+'.ko.gz')))
for i,p in enumerate(order):(I/'modules'/f'{i}.ko').write_bytes(gzip.decompress((M/p).read_bytes()))
(I/'load-modules').write_text('\n'.join('insmod /modules/%d.ko'%i for i in range(len(order)))+'\n');(D/'linux-modules.txt').write_text('\n'.join(order)+'\n')
(I/'init').write_text('''#!/bin/sh
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
. /load-modules
for i in 1 2 3 4 5;do [ ! -b /dev/vda1 ] || break;sleep 1;done
if ! mount -t ext2 -o rw /dev/vda1 /rootfs;then echo LINUX_TCG_FIXTURE_MOUNT_FAILED;poweroff -f;fi
mount -t proc proc /rootfs/proc
mount -t sysfs sysfs /rootfs/sys
mount -t devtmpfs devtmpfs /rootfs/dev
mkdir -p /rootfs/dev/pts /rootfs/dev/shm
mount -t devpts -o newinstance,ptmxmode=0666,mode=0620 devpts /rootfs/dev/pts
rm -f /rootfs/dev/ptmx
ln -s pts/ptmx /rootfs/dev/ptmx
mount -t tmpfs -o mode=1777 tmpfs /rootfs/dev/shm
mount -t tmpfs -o mode=1777 tmpfs /rootfs/tmp
mount -t tmpfs -o mode=755 tmpfs /rootfs/run
mount -t tmpfs -o mode=755 tmpfs /rootfs/home
mkdir -p /rootfs/home/user
chown 1000:1000 /rootfs/home/user
chmod 700 /rootfs/home/user
echo LINUX_PHYSICAL_ORACLE_KERNEL=Linux
exec chroot /rootfs /sbin/init
''');(I/'init').chmod(0o755)
subprocess.run('find . -print0 | cpio -0 -o -H newc --owner=0:0 --quiet | gzip -1 > '+str(D/'linux-init.cpio.gz'),cwd=I,shell=True,check=True)
for sample in ['linux1','guest1','guest2','guest3']:
 B=D/'adapters'/sample;B.mkdir(parents=True)
 if sample=='linux1':
  text='''#!/usr/bin/env python3
import os,sys,shlex
from pathlib import Path
T=Path(__file__).resolve().parents[4];D=T/'build/m40-cold'
a={v.split('=',1)[0]:v.split('=',1)[1]for v in sys.argv[1:]if '='in v}
if 'run'not in sys.argv or a.get('ARCH')!='x86_64':sys.exit('build/non-x86 forbidden')
q=[str(Path(os.environ['TOOLS_DIR'])/'bin/qemu-system-x86_64'),'-M','q35','-cpu','qemu64,+smep,+smap,+rdrand','-smp',a['SMP'],'-m',a['QEMU_MEMORY'],'-kernel',str(D/'linux/boot/vmlinuz-virt'),'-initrd',str(D/'linux-init.cpio.gz'),'-append','console=ttyS0 panic=-1 rdinit=/init','-snapshot','-device','virtio-keyboard-pci','-device','virtio-tablet-pci','-serial','stdio','-no-reboot']+shlex.split(a['QEMUFLAGS'])
os.execv(q[0],q)
'''
 else:
  text='''#!/bin/sh
case " $* " in *' ARCH=x86_64 '*) ;;*) exit 2;;esac
case " $* " in *' run '*) ;;*) exit 2;;esac
exec /usr/bin/make --old-file=build/x86_64-alpine/9os.iso --old-file=firmware "$@"
'''
 (B/'make').write_text(text);(B/'make').chmod(0o755)
subprocess.run(['make','-j2','ARCH=x86_64','USERLAND=alpine','CMDLINE=root=/dev/vda1','iso'],check=True)
subprocess.run(['scripts/mkdisk.sh',str(D/'desktop.img'),'3584',str(R)],check=True)
files=[D/'host-profile.json',D/'desktop.img',K/'boot/vmlinuz-virt',D/'linux-init.cpio.gz',D/'adapters/linux1/make',D/'adapters/guest1/make']
files += [TOP/'build/x86_64-alpine'/n for n in ['9os.iso','kernel.elf','initramfs.cpio','limine.conf']]
files += [R/'usr/bin'/n for n in ['alpine-plasma-probe','desktopa11y','desktopa11y-dbus','trace-snapshot','trace-snapshot-target','trace-snapshot-selftest','inputeventcapture','input-capture-session']]
files += [TOP/'scripts'/n for n in ['m40-cold-driver.sh','m40-cold-prepare.py','m40-cold-run.py','qemu-test.py','qemu-ready-type.py','qemu-type.py']]
manifest={str(p.relative_to(TOP)):hashlib.file_digest(p.open('rb'),'sha256').hexdigest()for p in files}
(D/'fixtures.json').write_text(json.dumps(manifest,indent=2)+'\n')
(D/'plan.json').write_text(json.dumps({'validation_head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'runtime_base':'935bd746cb6ea6d9ed833a5125b8f4465dc31ee7','fixed_samples':['linux1','guest1','guest2','guest3'],'retries':0,'host_profile':'two logical CPUs, recorded immutable affinity; no host memory cap','observers':'Qt/Wayland off; failure-only snapshots','expected_linux':'17/18; only missing 9os lockdep','expected_9os':'18/18; raw physical capture required'},indent=2)+'\n')
print('FIXED_COLD_FIXTURES_READY')
