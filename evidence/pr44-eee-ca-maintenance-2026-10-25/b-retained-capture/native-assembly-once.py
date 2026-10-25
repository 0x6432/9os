import pathlib,json,subprocess,datetime,time,hashlib,os
r=pathlib.Path('/data/ca-maintenance-product');e=pathlib.Path(__file__).parent;tools=pathlib.Path('/data/ca-maintenance-clean-tools-once');root=r/'userland/alpine-root-x86_64';work=r/'userland/build'
assert not tools.exists() and not root.exists() and not work.exists()
tools.mkdir(); assert list(tools.iterdir())==[]
proof={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'tools':str(tools),'tools_entries_before':0,'root_exists_before':root.exists(),'work_exists_before':work.exists(),'source_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip()}
(e/'native-empty-before.json').write_text(json.dumps(proof,indent=2)+'\n')
assert hashlib.sha256((r/'userland/alpine/build.py').read_bytes()).hexdigest()=='fe5b23dd4970b1423fd32c0dda02228619ac664c506e6b4f91ecef64075a4dae'
cmd=['timeout','--signal=TERM','--kill-after=10s','600s','env','TOOLS_DIR='+str(tools),'PYTHONDONTWRITEBYTECODE=1','python',str(r/'userland/alpine/build.py'),'--arch','x86_64','--profile','base','--lock',str(r/'userland/alpine/locks/x86_64.json')]
(e/'native-argv.json').write_text(json.dumps(cmd,indent=2)+'\n');start=time.monotonic()
with (e/'native.stdout').open('wb')as out,(e/'native.stderr').open('wb')as err: result=subprocess.run(cmd,cwd=r,stdout=out,stderr=err)
status={'returncode':result.returncode,'elapsed_s':round(time.monotonic()-start,3),'utc_end':datetime.datetime.now(datetime.timezone.utc).isoformat()};(e/'native-status.json').write_text(json.dumps(status,indent=2)+'\n'); print(json.dumps(status));raise SystemExit(result.returncode)
