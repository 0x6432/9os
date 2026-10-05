import socket,sys,time
s=socket.socket(socket.AF_UNIX); s.connect('/tmp/mon.sock'); time.sleep(0.3); s.recv(65536)
for c in sys.argv[1:]:
    s.send((c+'\n').encode()); time.sleep(1.5)
    out=b''
    s.settimeout(1)
    try:
        while True:
            d=s.recv(65536)
            if not d: break
            out+=d
    except Exception: pass
    sys.stdout.write(out.decode(errors='replace'))
s.close()
