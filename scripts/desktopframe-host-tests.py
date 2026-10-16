#!/usr/bin/env python3
"""Host controls for the guest's readiness poll, not replacement desktop evidence."""
import os,pathlib,subprocess,tempfile
TOP=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as d:
 d=pathlib.Path(d);exe=d/'check'
 subprocess.run([os.environ.get('CC','cc'),'-O2','-Wall','-Wextra',str(TOP/'userland/tests/desktopframecheck.c'),'-o',str(exe)],check=True)
 def run(data,code):
  f=d/'frame.ppm';f.write_bytes(data);r=subprocess.run([str(exe),str(f)],capture_output=True)
  assert r.returncode==code,(r.returncode,code,r.stdout,r.stderr)
 header=b'P6\n# compositor output\n800 600\n255\n'
 black=bytes(800*600*3);run(header+black,1)
 pattern=bytes(c for y in range(600) for x in range(800) for c in (x%256,y%256,(x+y)%256))
 run(header+pattern,0)
 # Panel-only, wallpaper-only, empty, wrong format, truncated and extra bytes.
 run(header+black[:800*550*3]+pattern[800*550*3:],1)
 run(header+pattern[:800*550*3]+black[800*550*3:],1)
 for data in [b'',b'P3\n800 600\n255\n'+pattern,header+pattern[:-1],header+pattern+b'junk',b'P6\n999999 1\n255\n']:
  run(data,2)
print('PASS: desktop frame readiness positive and eight negative controls')
