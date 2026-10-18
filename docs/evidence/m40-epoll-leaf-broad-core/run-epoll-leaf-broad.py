import os,pathlib,json,hashlib,subprocess,time,stat,runpy,shutil
repo=pathlib.Path('/data/9os');ev=pathlib.Path('/data/9os-failure-phase-evidence/docs/evidence/m40-epoll-leaf-broad-core');ev.mkdir(parents=True,exist_ok=True);os.chdir(repo)
head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip();assert head=='cdfd3a5173837c4713ec577248b1a7bd36ebe6b4'
cpus=sorted(os.sched_getaffinity(0));selected=cpus[:2]
plan={'head':head,'command':['scripts/ci-tests.sh','x86_64','--boot-timeout','300'],'attempts':1,'retries':0,'host_selected_cpus':selected,'scope':'Separate CORE ONLY standard multi-boot suite. NORMAL make, no frozen-image adapter. ISOs may repack BETWEEN boots for CMDLINE. Whole-fixture equality is recorded, NOT required or claimed. Kernel/initramfs/root/driver invariants required. No overlapping local VM/heavy build. No graphics/cold validation.','created':time.time()}
(ev/'plan.json').write_text(json.dumps(plan,indent=2)+'\n');shutil.copy2(__file__,ev/'run-epoll-leaf-broad.py')
files=['build/x86_64/9os.iso','build/x86_64/kernel.elf','build/x86_64/initramfs.cpio','Makefile','scripts/ci-tests.sh','scripts/qemu-test.py','scripts/snd-qemu-tests.sh','userland/root-x86_64/bin/pollwakebench','userland/root-x86_64/bin/pollfiltertest','userland/root-x86_64/bin/unixpolltest']
def digest():
 d={f:hashlib.sha256(pathlib.Path(f).read_bytes()).hexdigest() for f in files};root=repo/'userland/root-x86_64';rows=[]
 for p in sorted(root.rglob('*')):
  st=p.lstat();rows.append((str(p.relative_to(root)),stat.S_IMODE(st.st_mode),os.readlink(p) if p.is_symlink() else hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else 'directory'))
 d['whole-root-manifest']=hashlib.sha256(json.dumps(rows).encode()).hexdigest();return d
before=digest();(ev/'before.json').write_text(json.dumps(before,indent=2)+'\n')
subprocess.run(['git','add',str(ev)],cwd='/data/9os-failure-phase-evidence',check=True)
subprocess.run(['git','-c','user.name=0x6432','-c','user.email=0x6432@users.noreply.github.com','commit','-m','Predeclare separate standard multi-boot leaf epoll core regression'],cwd='/data/9os-failure-phase-evidence',check=True)
os.chdir('/data/9os-failure-phase-evidence');runpy.run_path('/tmp/9os-gh.py')['push']('notion/m40-failure-phase-evidence');os.chdir(repo)
os.sched_setaffinity(0,selected);env=os.environ.copy();env['PATH']='/data/tools/bin:'+env['PATH']
with (ev/'driver.out').open('wb') as out:rc=subprocess.run(plan['command'],env=env,stdout=out,stderr=subprocess.STDOUT).returncode
after=digest();changed=[f for f in before if before[f]!=after[f]];invariants=all(f=='build/x86_64/9os.iso' for f in changed)
(ev/'after.json').write_text(json.dumps(after,indent=2)+'\n')
for p in (repo/'build').glob('test*x86_64*.log'):shutil.copy2(p,ev/p.name)
for p in (repo/'build/ci-x86_64').glob('*.fsck'):shutil.copy2(p,ev/p.name)
result={'driver_rc':rc,'whole_fixture_equal':before==after,'changed':changed,'input_invariants_valid':invariants,'accepted':rc==0 and invariants,'finished':time.time()};(ev/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True);raise SystemExit(0 if result['accepted'] else 1)
