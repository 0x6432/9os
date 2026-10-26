from pathlib import Path
p=Path('/data/linux-oracle-cache-product/scripts/evdev-linux-oracle-packages-host-tests.py');s=p.read_text()
old='import importlib.util,json,os,pathlib,sys\nhelper,tools,target,manifest,trace=sys.argv[1:6]\n'
new='''import ctypes,importlib.util,json,os,pathlib,sys
def privilege_status():
    wanted={'Uid','Gid','Groups','CapEff','CapPrm','CapInh','CapAmb','CapBnd','NoNewPrivs'}
    return {line.split(':',1)[0]:line.split(':',1)[1].strip()
            for line in pathlib.Path('/proc/self/status').read_text().splitlines()
            if line.split(':',1)[0] in wanted}
def require_unprivileged(status):
    assert os.geteuid()!=0 and os.getegid()!=0
    assert status['NoNewPrivs']=='1'
    assert all(int(status[key],16)==0 for key in ['CapEff','CapPrm','CapInh','CapAmb'])
if os.environ.get('ORACLE_TEST_ZERO_CAP_EXEC')!='1':
    before=privilege_status()
    libc=ctypes.CDLL(None,use_errno=True)
    assert libc.prctl(38,1,0,0,0)==0, ctypes.get_errno()  # PR_SET_NO_NEW_PRIVS
    assert libc.prctl(47,4,0,0,0)==0, ctypes.get_errno()  # PR_CAP_AMBIENT_CLEAR_ALL
    class Header(ctypes.Structure):
        _fields_=[('version',ctypes.c_uint32),('pid',ctypes.c_int)]
    class Data(ctypes.Structure):
        _fields_=[('effective',ctypes.c_uint32),('permitted',ctypes.c_uint32),('inheritable',ctypes.c_uint32)]
    header=Header(0x20080522,0)
    data=(Data*2)()
    assert libc.capset(ctypes.byref(header),ctypes.byref(data))==0, ctypes.get_errno()
    after=privilege_status();require_unprivileged(after)
    env={**os.environ,'ORACLE_TEST_ZERO_CAP_EXEC':'1',
         'ORACLE_TEST_CAP_BEFORE':json.dumps(before),'ORACLE_TEST_CAP_AFTER_DROP':json.dumps(after)}
    os.execve(sys.executable,[sys.executable,'-c',env['ORACLE_TEST_CHILD_CODE'],*sys.argv[1:]],env)
post=privilege_status();require_unprivileged(post)
helper,tools,target,manifest,trace=sys.argv[1:6]
# A genuine denial must precede the helper witness, not an EUID-only assumption.
try:pathlib.Path(tools,'dl','permission-probe').mkdir()
except PermissionError:pass
else:raise AssertionError('Read-only dl remains writable; invalid permission environment')
'''
assert s.count(old)==1;s=s.replace(old,new)
s=s.replace("'dl_mode':oct(pathlib.Path(tools,'dl').stat().st_mode & 0o777)}),flush=True)","'dl_mode':oct(pathlib.Path(tools,'dl').stat().st_mode & 0o777),\n                  'before_exec':json.loads(os.environ['ORACLE_TEST_CAP_BEFORE']),\n                  'after_drop':json.loads(os.environ['ORACLE_TEST_CAP_AFTER_DROP']),\n                  'post_exec':post}),flush=True)")
s=s.replace("env={**os.environ, 'TOOLS_DIR': str(tools), 'PYTHONDONTWRITEBYTECODE': '1'},","env={**os.environ, 'TOOLS_DIR': str(tools), 'PYTHONDONTWRITEBYTECODE': '1',\n                                     'ORACLE_TEST_CHILD_CODE': child, 'ORACLE_TEST_ZERO_CAP_EXEC': '0'},")
needle="        self.assertNotEqual(observation['egid'], 0)\n";s=s.replace(needle,needle+"        self.assertEqual(observation['post_exec']['NoNewPrivs'], '1')\n        for key in ['CapEff', 'CapPrm', 'CapInh', 'CapAmb']:\n            self.assertEqual(int(observation['post_exec'][key], 16), 0)\n")
p.write_text(s)
