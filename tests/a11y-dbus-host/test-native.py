from pathlib import Path
import os,subprocess,shlex,signal,select,tempfile
D=Path(__file__).resolve().parent
# This uses a signed prebuilt native x86 daemon and genuine public GIO IPC, not
# mocked GIO symbols; the server is explicitly synthetic and never GUI proof.
def ready(p,prefix,timeout=5):
 if not select.select([p.stdout],[],[],timeout)[0]:raise RuntimeError('startup timeout')
 line=p.stdout.readline().strip()
 assert line.startswith(prefix),(line,p.stderr.read()if p.poll()is not None else'')
 return line
with tempfile.TemporaryDirectory(prefix='9os-private-a11y-')as tmp:
 t=Path(tmp);cfg=t/'bus.conf';cfg.write_text('''<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd"><busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth><policy context="default"><allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>''')
 s=Path('/data/9os/userland/alpine-root-x86_64')
 daemon=subprocess.Popen([str(s/'lib/ld-musl-x86_64.so.1'),'--library-path',str(s/'lib')+':'+str(s/'usr/lib'),str(s/'usr/bin/dbus-daemon'),'--nofork','--print-address=1','--config-file='+str(cfg)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
 try:
  addr=ready(daemon,'unix:');env={**os.environ,'DBUS_SESSION_BUS_ADDRESS':addr}
  cases=[('valid',0),('wrong-pid',1),('unfocused',1),('hidden',1),('invisible',1),('wrong-role',1),('short-state',1),('long-state',1),('bad-address',1),('empty',1),('duplicate-app',1),('many-apps',1),('many-children',1),('bad-path',1),('foreign-child',1),('cycle',1),('menu-decoy',1),('remote-error',1),('blocked-role',1)]
  for mode,expected in cases:
   server=subprocess.Popen([str(D/'mock-server'),mode],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
   try:
    ready(server,'MOCK_SERVER_READY');pid=server.pid+(1 if mode=='wrong-pid'else 0)
    result=subprocess.run([str(D/'terminal-dbus'),str(pid)],env=env,text=True,capture_output=True,timeout=10)
    (D/('native-'+mode+'.out')).write_text(result.stdout+result.stderr)
    assert result.returncode==expected,(mode,result.returncode,result.stdout,result.stderr)
    if mode=='valid':assert 'DBUS_A11Y_TERMINAL focused=1 showing=1 visible=1'in result.stdout
    if mode=='menu-decoy':assert 'DBUS_A11Y_TERMINAL 'not in result.stdout
    print('PASS synthetic protocol',mode,flush=True)
   finally:server.terminate();server.communicate(timeout=5)
  for pid in ['','0','-1','1x','2147483648']:
   result=subprocess.run([str(D/'terminal-dbus'),pid],env=env,capture_output=True,timeout=5);assert result.returncode==2
  print('19/19 real private IPC controls + invalid PID bounds; synthetic only, not Qt or GUI proof')
 finally:daemon.terminate();daemon.communicate(timeout=5)
