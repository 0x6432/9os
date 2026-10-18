import os,pathlib,json,hashlib,subprocess,time,stat
repo=pathlib.Path('/data/9os');ev=pathlib.Path('/data/9os-failure-phase-evidence/docs/evidence/m40-epoll-wake-baseline');ev.mkdir(parents=True,exist_ok=True)
os.chdir(repo)
cpus=sorted(os.sched_getaffinity(0));selected=cpus[:2]
modes=['','--inotify','--unix','--unix-inotify','--epoll','--epoll-inotify','--epoll-unix','--epoll-unix-inotify']
cmds=['pollwakebench'+(' '+m if m else '') for m in modes]
cmds+=['su user -s /bin/sh -c "'+c+'"' for c in cmds.copy()]
cmds+=['unixpolltest','su user -s /bin/sh -c unixpolltest','pollfiltertest','su user -s /bin/sh -c pollfiltertest','socktest','ipctest','polltest',"grep -q '^violations 0' /proc/lockdep"]
plan={'runtime_source':'e0bc041024445770aad61fa70824527b0395a58f','measurement_head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'samples':1,'retries':0,'guest_arch':'x86_64','guest_smp':4,'guest_memory':'512M','host_available_cpus':cpus,'host_selected_cpus':selected,'commands':cmds,'boot_timeout':120,'command_timeout':60,'scope':'CORE ONLY. Informational unchanged-kernel measurement, not graphics/cold validation. No performance thresholds.','created':time.time()}
assert len(cmds)==24
(ev/'plan.json').write_text(json.dumps(plan,indent=2)+'\n')
files=['build/x86_64/9os.iso','build/x86_64/kernel.elf','build/x86_64/initramfs.cpio','Makefile','scripts/qemu-test.py','userland/tests/pollwakebench.c']+['userland/root-x86_64/bin/'+f for f in ['pollwakebench','unixpolltest','pollfiltertest','socktest','ipctest','polltest','busybox']]
def digest():
 d={f:hashlib.sha256(pathlib.Path(f).read_bytes()).hexdigest() for f in files}
 root=repo/'userland/root-x86_64';manifest=[]
 for p in sorted(root.rglob('*')):
  st=p.lstat();manifest.append((str(p.relative_to(root)),stat.S_IMODE(st.st_mode),os.readlink(p) if p.is_symlink() else hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else 'directory'))
 d['whole-root-manifest']=hashlib.sha256(json.dumps(manifest).encode()).hexdigest();return d
before=digest();(ev/'fixtures-before.json').write_text(json.dumps(before,indent=2)+'\n')
subprocess.run(['git','add',str(ev)],cwd='/data/9os-failure-phase-evidence',check=True)
subprocess.run(['git','-c','user.name=0x6432','-c','user.email=0x6432@users.noreply.github.com','commit','-m','Predeclare one frozen x86 direct epoll unchanged-kernel measurement'],cwd='/data/9os-failure-phase-evidence',check=True)
env=os.environ.copy();env['PATH']='/data/tools/bin:'+env['PATH'];os.sched_setaffinity(0,selected)
command=['python3','scripts/qemu-test.py','x86_64','--memory','512M','--smp','4','--boot-timeout','120','--timeout','60','--log',str(ev/'guest.log')]+cmds
with (ev/'driver.out').open('wb') as out: rc=subprocess.run(command,env=env,stdout=out,stderr=subprocess.STDOUT).returncode
after=digest();(ev/'fixtures-after.json').write_text(json.dumps(after,indent=2)+'\n')
result={'driver_rc':rc,'fixture_equal':before==after,'changed':[f for f in before if before[f]!=after[f]],'finished':time.time(),'accepted':rc==0 and before==after}
(ev/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result));raise SystemExit(0 if result['accepted'] else 1)
