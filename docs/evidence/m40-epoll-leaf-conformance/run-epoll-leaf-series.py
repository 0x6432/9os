import os,pathlib,json,hashlib,subprocess,time,stat,runpy,shutil
repo=pathlib.Path('/data/9os');ev=pathlib.Path('/data/9os-failure-phase-evidence/docs/evidence/m40-epoll-leaf-conformance');ev.mkdir(parents=True,exist_ok=True);os.chdir(repo)
head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip();assert head=='cdfd3a5173837c4713ec577248b1a7bd36ebe6b4'
assert not subprocess.check_output(['git','status','--porcelain'],text=True)
cpus=sorted(os.sched_getaffinity(0));selected=cpus[:2]
modes=['','--inotify','--unix','--unix-inotify','--epoll','--epoll-inotify','--epoll-unix','--epoll-unix-inotify']
cmds=['pollwakebench'+(' '+m if m else '') for m in modes];cmds+=['su user -s /bin/sh -c "'+c+'"' for c in cmds.copy()]
cmds+=['pollfiltertest','su user -s /bin/sh -c pollfiltertest','unixpolltest','su user -s /bin/sh -c unixpolltest','socktest','ipctest','polltest',"grep -q '^violations 0' /proc/lockdep"]
plan={'head':head,'guest_arch':'x86_64','guest_memory':'512M','guest_smp_samples':[1,4,8],'sample_count':3,'retries':0,'host_available_cpus':cpus,'host_selected_cpus':selected,'commands':cmds,'boot_timeout':120,'command_timeout':60,'scope':'CORE ONLY, no graphics/cold proof. New restored host fixture, NOT matched-host performance comparison. All samples run independently once even after a failure.','created':time.time()}
assert len(cmds)==24
(ev/'plan.json').write_text(json.dumps(plan,indent=2)+'\n');shutil.copy2(__file__,ev/'run-epoll-leaf-series.py')
files=['build/x86_64/9os.iso','build/x86_64/kernel.elf','build/x86_64/initramfs.cpio','Makefile','scripts/qemu-test.py','kernel/fs/anonfd.c','kernel/fs/vfs.c','kernel/core/sched.c','userland/tests/pollfiltertest.c','userland/tests/unixpolltest.c']+['userland/root-x86_64/bin/'+n for n in ['pollwakebench','pollfiltertest','unixpolltest','socktest','ipctest','polltest','busybox']]
def digest():
 d={f:hashlib.sha256(pathlib.Path(f).read_bytes()).hexdigest() for f in files};root=repo/'userland/root-x86_64';manifest=[]
 for p in sorted(root.rglob('*')):
  st=p.lstat();manifest.append((str(p.relative_to(root)),stat.S_IMODE(st.st_mode),os.readlink(p) if p.is_symlink() else hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else 'directory'))
 d['whole-root-manifest']=hashlib.sha256(json.dumps(manifest).encode()).hexdigest();return d
before=digest();(ev/'series-before.json').write_text(json.dumps(before,indent=2)+'\n')
subprocess.run(['git','add',str(ev)],cwd='/data/9os-failure-phase-evidence',check=True)
subprocess.run(['git','-c','user.name=0x6432','-c','user.email=0x6432@users.noreply.github.com','commit','-m','Predeclare fixed epoll leaf SMP1/SMP4/SMP8 conformance series'],cwd='/data/9os-failure-phase-evidence',check=True)
os.chdir('/data/9os-failure-phase-evidence');runpy.run_path('/tmp/9os-gh.py')['push']('notion/m40-failure-phase-evidence');os.chdir(repo)
env=os.environ.copy();env['PATH']='/data/tools/bin:'+env['PATH'];os.sched_setaffinity(0,selected);results=[]
for smp in plan['guest_smp_samples']:
 sample_before=digest();(ev/f'smp{smp}-before.json').write_text(json.dumps(sample_before,indent=2)+'\n')
 command=['python3','scripts/qemu-test.py','x86_64','--memory','512M','--smp',str(smp),'--boot-timeout','120','--timeout','60','--log',str(ev/f'smp{smp}.log')]+cmds
 with (ev/f'smp{smp}.out').open('wb') as out:rc=subprocess.run(command,env=env,stdout=out,stderr=subprocess.STDOUT).returncode
 after=digest();(ev/f'smp{smp}-after.json').write_text(json.dumps(after,indent=2)+'\n')
 result={'smp':smp,'driver_rc':rc,'fixture_before':sample_before==before,'fixture_after':after==before,'changed':[f for f in before if before[f]!=after[f]],'accepted':rc==0 and sample_before==before and after==before,'finished':time.time()};results.append(result)
 (ev/f'smp{smp}-result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
final=digest();result={'samples':results,'all_accepted':all(r['accepted'] for r in results),'series_fixture_equal':before==final,'finished':time.time()};(ev/'series-result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True);raise SystemExit(0 if result['all_accepted'] and before==final else 1)
