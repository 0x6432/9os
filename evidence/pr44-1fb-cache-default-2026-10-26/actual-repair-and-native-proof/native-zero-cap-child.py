import ctypes,json,os,pathlib,runpy,sys
E=pathlib.Path('/data/signed-1fb-review/cache-repair')
def status():
 wanted={'Uid','Gid','Groups','CapEff','CapPrm','CapInh','CapAmb','CapBnd','NoNewPrivs'}
 return {l.split(':',1)[0]:l.split(':',1)[1].strip()for l in pathlib.Path('/proc/self/status').read_text().splitlines()if l.split(':',1)[0]in wanted}
def validate(s):
 assert os.geteuid()!=0 and os.getegid()!=0
 assert s['NoNewPrivs']=='1'
 assert all(int(s[k],16)==0 for k in ['CapEff','CapPrm','CapInh','CapAmb'])
if os.environ.get('ORACLE_NATIVE_ZERO_CAP_EXEC')!='1':
 before=status();libc=ctypes.CDLL(None,use_errno=True)
 assert libc.prctl(38,1,0,0,0)==0,ctypes.get_errno()
 assert libc.prctl(47,4,0,0,0)==0,ctypes.get_errno()
 class Header(ctypes.Structure):_fields_=[('version',ctypes.c_uint32),('pid',ctypes.c_int)]
 class Data(ctypes.Structure):_fields_=[('effective',ctypes.c_uint32),('permitted',ctypes.c_uint32),('inheritable',ctypes.c_uint32)]
 h=Header(0x20080522,0);d=(Data*2)();assert libc.capset(ctypes.byref(h),ctypes.byref(d))==0,ctypes.get_errno()
 after=status();validate(after)
 env={**os.environ,'ORACLE_NATIVE_ZERO_CAP_EXEC':'1','ORACLE_NATIVE_BEFORE':json.dumps(before),'ORACLE_NATIVE_AFTER_DROP':json.dumps(after)}
 os.execve(sys.executable,[sys.executable,__file__],env)
post=status();validate(post)
tools=pathlib.Path('/data/linux-oracle-zero-cap-tools-once');target=pathlib.Path('/data/linux-oracle-zero-cap-target-once')
assert os.environ['TOOLS_DIR']==str(tools);assert tools.stat().st_uid==os.geteuid();assert not any(target.iterdir())
try:(tools/'dl/permission-probe').mkdir()
except PermissionError:pass
else:raise AssertionError('Invalid privilege context: readonly dl writable')
proof={'before_exec':json.loads(os.environ['ORACLE_NATIVE_BEFORE']),'after_drop':json.loads(os.environ['ORACLE_NATIVE_AFTER_DROP']),'post_exec':post,'euid':os.geteuid(),'egid':os.getegid(),'tools':str(tools),'default_cache':str(tools/'linux-oracle'),'dl_mode':oct((tools/'dl').stat().st_mode&0o777),'dl_uid':(tools/'dl').stat().st_uid,'target':str(target),'target_entries_before':0,'readonly_dl_probe':'genuine PermissionError','actual_python_entry':'runpy.run_path executes unchanged production main in THIS post-exec zero-cap process'}
(E/'native-zero-cap-proof.json').write_text(json.dumps(proof,indent=2)+'\n')
helper='/data/linux-oracle-cache-product/scripts/evdev-linux-oracle-packages.py'
sys.argv=[helper,'--target',str(target),'--manifest',str(E/'native-result-manifest.json'),'--offline']
assert '--cache'not in sys.argv;(E/'native-helper-sysargv.json').write_text(json.dumps(sys.argv,indent=2)+'\n')
runpy.run_path(helper,run_name='__main__')
