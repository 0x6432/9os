#!/usr/bin/env python3
"""Exercise runner control flow with an inert stub; never invokes make or QEMU."""
import hashlib, json, os, pathlib, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

source = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1] / 'scripts/qemu-test.py').read_text()
launch = "    make = ['make',"
spawn = '    try:\n        p = subprocess.Popen(make, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,\n'
assert source.count(launch) == source.count(spawn) == source.count('    server = None\n') == 1
stub = '''import os,sys,re,time
mode=os.environ.get('INERT_MODE','success')
if mode=='boot-exit': os._exit(7)
print('Welcome to 9os\\n# ',end='',flush=True)
for line in sys.stdin:
 if line.strip()=='poweroff': break
 m=re.search(r'__RC(\\d+)=',line)
 if not m: print('# ',end='',flush=True); continue
 if mode.startswith('echoed'): print(line.rstrip(),flush=True)
 if mode=='prefix': print('FAIL before '+line.rstrip(),flush=True)
 if mode=='suffix': print(line.rstrip()+' FAIL after',flush=True)
 if mode=='timeout': time.sleep(2); continue
 if mode=='rc-prefix': print('prefix __RC%s=0' % m.group(1),flush=True); time.sleep(2); continue
 if mode=='panic': print('kernel panic: FAKE CONTROL',flush=True); break
 if mode in ('failword','echoed-failword'): print('FAKE FAIL diagnostic')
 if mode not in ('prefix','suffix'): print('FAKE CONTROL ONLY')
 print('__RC%s=%s\\n# ' % (m.group(1),'1' if mode=='nonzero' else '0'),end='',flush=True)
 if mode=='early-exit': os._exit(7)
'''
with tempfile.TemporaryDirectory(prefix='runner-inert-regression-') as td:
    p=pathlib.Path(td); fake=p/'fake.py'; fake.write_text(stub); disk=p/'inert.raw';disk.write_bytes(b'INERT TEST DISK\n');seed=hashlib.sha256(disk.read_bytes()).hexdigest()
    guard="    if a.net_test or any(c.startswith('host:') for c in a.cmds):\n        raise SystemExit('REFUSED: inert test prohibits network and host commands')\n    server = None\n"
    text=source.replace('    server = None\n',guard)
    a=text.index(launch); b=text.index(spawn,a)
    replacement=('    # INERT control: only a fixed Python stub and inert disk are permitted.\n'
                 '    from pathlib import Path\n'
                 f'    fixed_disk = Path({str(disk)!r})\n'
                 "    if len(a.disk) != 1 or Path(a.disk[0]) != fixed_disk or fixed_disk.is_symlink() or fixed_disk.read_bytes() != b'INERT TEST DISK\\n':\n"
                 "        raise SystemExit('REFUSED: inert disk only')\n"
                 f'    make = [sys.executable, {str(fake)!r}]\n')
    text=text[:a]+replacement+text[b:]
    assert launch not in text
    runner=p/'runner.py';runner.write_text(text)
    results=[]
    modes=('success','nonzero','failword','panic','timeout','echoed','echoed-failword','prefix','suffix','rc-prefix','early-exit','boot-exit')
    def run_mode(mode):
        cmds=['echo FAIL' if mode=='echoed' else 'echo harmless']
        if mode in ('early-exit','boot-exit','panic'):cmds+=['echo second']
        r=subprocess.run([sys.executable,str(runner),'x86_64','--userland','alpine','--disk',str(disk),'--boot-timeout','10','--timeout','0.3',*cmds],env=dict(os.environ,INERT_MODE=mode),capture_output=True,timeout=40)
        output=(r.stdout+r.stderr).decode(errors='replace'); expected=0 if mode in ('success','echoed') else 1
        ok=r.returncode==expected
        if mode in ('early-exit','boot-exit','panic'):ok &= 'SKIP   echo second' in output
        if mode=='early-exit':ok &= 'FAIL   VM exited before requested shutdown' in output and 'PASS   echo harmless' in output
        if mode=='boot-exit':ok &= 'FAIL   boot' in output and 'SKIP   echo harmless' in output
        if mode in ('prefix','suffix','rc-prefix','failword','echoed-failword'):ok &= 'FAIL   echo harmless' in output
        assert hashlib.sha256(disk.read_bytes()).hexdigest()==seed
        result={'mode':mode,'rc':r.returncode,'pass':bool(ok)}
        if not ok: result['failure_output']=output
        return result
    with ThreadPoolExecutor(max_workers=4) as pool:
        mode_results=list(pool.map(run_mode, modes))
    for result in mode_results:
        results.append(result)
        print(result['mode'], "PASS" if result['pass'] else "FAIL",flush=True)
        if not result['pass']: print(result.get('failure_output',''),flush=True)
    for extra in (['--net-test'], ['host:echo unsafe'], ['--disk','/dev/null']):
        r=subprocess.run([sys.executable,str(runner),'x86_64','--userland','alpine','--disk',str(disk),*extra],capture_output=True,timeout=10)
        ok=r.returncode!=0 and b'REFUSED:' in r.stderr
        results.append({'negative':extra,'rc':r.returncode,'pass':ok});print('INERT-ONLY test guard rejection',extra,'PASS' if ok else 'FAIL',flush=True)
    # Inert-only non-frozen control: a failed pre-type screenshot must stop
    # the next host command. Only these two exact host commands are permitted.
    scripts=p/'scripts';scripts.mkdir();(scripts/'qemu-screenshot.py').write_text('raise SystemExit(7)\n')
    shot='host:python3 scripts/qemu-screenshot.py unused unused'
    later='host:touch '+str(p/'should-not-run')
    guard2=('    if a.net_test or any(c.startswith("host:") and c not in '+repr([shot,later])+ ' for c in a.cmds):\n'
            '        raise SystemExit("REFUSED: inert test permits only its two host controls")\n    server = None\n')
    text2=source.replace('    server = None\n',guard2);a=text2.index(launch);b=text2.index(spawn,a);text2=text2[:a]+replacement+text2[b:]
    assert launch not in text2
    runner2=p/'runner-shot.py';runner2.write_text(text2)
    r=subprocess.run([sys.executable,str(runner2),'x86_64','--userland','alpine','--disk',str(disk),'--boot-timeout','10','--timeout','3',shot,later],cwd=p,env=dict(os.environ,INERT_MODE='success'),capture_output=True,timeout=30)
    output=(r.stdout+r.stderr).decode(errors='replace');ok=r.returncode!=0 and 'FAIL   '+shot in output and 'SKIP   '+later in output and not (p/'should-not-run').exists()
    results.append({'mode':'failed-pretype-screenshot','rc':r.returncode,'pass':bool(ok)})
    print('failed-pretype-screenshot', 'PASS' if ok else 'FAIL',flush=True)
    if not ok:print(output,flush=True)
    print(json.dumps(results),flush=True)
    if not all(x['pass'] for x in results):raise SystemExit(1)
