#!/usr/bin/env python3
"""Host side of the M32 network tests (started by scripts/qemu-test.py --net-test).

usage: net-host-server.py PORT
TCP on 127.0.0.1:PORT (the guest reaches it as 10.0.2.2:PORT through QEMU user networking):
  "GET /hello"  -> HTTP/1.0 200, body "hello from the host\\n"
  "GET /big"    -> HTTP/1.0 200, 1 MiB body, byte i = (i * 7 + i // 251) & 0xff  (md5 printed by --md5)
  other GET     -> 404
  anything else -> echoed back until the client shuts down its side
UDP on the same port: every datagram is echoed. Both also listen on [::1]:PORT (guest: [fec0::2]:PORT).
net-host-server.py --md5             print the md5 of the /big body
net-host-server.py --echo-check PORT send /big's bytes to 127.0.0.1:PORT and expect them echoed back"""
import hashlib, socket, sys, threading

BIG = bytes(((i * 7 + i // 251) & 0xff) for i in range(1 << 20))

def tcp_client(c):
    with c:
        first = c.recv(65536)
        if first.startswith(b'GET '):
            while b'\r\n\r\n' not in first and b'\n\n' not in first:
                d = c.recv(65536)
                if not d: break
                first += d
            path = first.split()[1].decode(errors='replace')
            if path == '/hello': body, st = b'hello from the host\n', '200 OK'
            elif path == '/big': body, st = BIG, '200 OK'
            else: body, st = b'not found\n', '404 Not Found'
            c.sendall(f'HTTP/1.0 {st}\r\nContent-Type: application/octet-stream\r\nContent-Length: {len(body)}\r\n\r\n'.encode() + body)
            return
        d = first
        while d:
            c.sendall(d)
            d = c.recv(65536)

def tcp_server(port, fam=socket.AF_INET, addr='127.0.0.1'):
    s = socket.socket(fam); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((addr, port)); s.listen(16)
    while True:
        c, _ = s.accept()
        threading.Thread(target=tcp_client, args=(c,), daemon=True).start()

def udp_server(port, fam=socket.AF_INET, addr='127.0.0.1'):
    u = socket.socket(fam, socket.SOCK_DGRAM)
    u.bind((addr, port))
    while True:
        d, a = u.recvfrom(65536)
        u.sendto(d, a)

if __name__ == '__main__':
    if sys.argv[1] == '--md5': print(hashlib.md5(BIG).hexdigest()); sys.exit(0)
    if sys.argv[1] == '--echo-check':           # drive a guest echo server through a forwarded port
        c = socket.create_connection(('127.0.0.1', int(sys.argv[2])), 20); c.settimeout(30)
        threading.Thread(target=lambda: (c.sendall(BIG), c.shutdown(socket.SHUT_WR)), daemon=True).start()
        got = bytearray()
        while True:
            d = c.recv(65536)
            if not d: break
            got += d
        ok = got == BIG
        print(f'echo-check: {len(got)} of {len(BIG)} bytes {"ok" if ok else "FAIL"}'); sys.exit(0 if ok else 1)
    port = int(sys.argv[1])
    threading.Thread(target=udp_server, args=(port,), daemon=True).start()
    try:                                        # IPv6 too (M32b): the guest reaches ::1 as fec0::2
        for f in (tcp_server, udp_server):
            threading.Thread(target=f, args=(port, socket.AF_INET6, '::1'), daemon=True).start()
    except OSError: pass
    tcp_server(port)
