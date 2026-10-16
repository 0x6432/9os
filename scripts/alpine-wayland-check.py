#!/usr/bin/env python3
"""Verify Sway output pixels and each client's completed Wayland frame requests."""
import base64, io, pathlib, re, sys
from PIL import Image

def check(raw, renderer="software"):
    if renderer not in ("software", "opengl"):
        raise ValueError("Unknown renderer")
    blocks=re.findall(rb'ALPINE_WAYLAND_PNG_BEGIN\r?\n(.*?)ALPINE_WAYLAND_PNG_END',raw,re.S)
    traces=re.findall(rb'ALPINE_WAYLAND_TRACE_BEGIN uid=(0|1000)\r?\n(.*?)ALPINE_WAYLAND_TRACE_END',raw,re.S)
    if not blocks or len(traces)!=len(blocks):
        raise ValueError('Missing/mismatched compositor captures and per-client traces')
    completed=[]
    for i,(block,(uid,trace)) in enumerate(zip(blocks,traces,strict=True)):
        im=Image.open(io.BytesIO(base64.b64decode(re.sub(rb'\s',b'',block),validate=True))).convert('RGBA')
        # Mesa's tested swrast EGL config is RGB565. Preserve the software
        # backend's exact RGBA8888 assertion; GL quantization is NOT a tolerance.
        color=(17,170,85,255) if renderer=="software" else (16,170,82,255)
        if im.size!=(128,96) or im.getextrema()!=tuple((v,v) for v in color):
            raise ValueError(f'Wrong compositor capture {i}: {im.size}, {im.getextrema()}')
        if re.findall(rb'^ALPINE_WAYLAND_CLIENT_UID=(\d+)\r?$',trace,re.M)!=[uid]:
            raise ValueError(f'Client {i} did not run with expected UID {uid.decode()}')
        if renderer=="opengl":
            if not all(x in trace for x in [b"OpenGL",b"llvmpipe",b"QRhi"]):
                raise ValueError(f"Client {i} has no real llvmpipe OpenGL QRhi evidence")
            if not re.search(rb'wl_shm_pool[#@]\d+\.create_buffer\(new id wl_buffer[#@]\d+, 0, 128, 96, 256, 909199186\)',trace):
                raise ValueError(f"Client {i} has no RGB565 EGL buffer evidence")
        # Callback object IDs are reused. A done event counts ONLY after a new
        # surface.frame request for that ID; display.sync callbacks don't count.
        pending=set();done=0
        for line in trace.splitlines():
            request=re.search(rb'wl_surface[#@]\d+\.frame\(new id wl_callback[#@](\d+)\)',line)
            event=re.search(rb'wl_callback[#@](\d+)\.done\(',line)
            if request:pending.add(request.group(1))
            if event and event.group(1) in pending:
                pending.remove(event.group(1));done+=1
        if done<3:
            raise ValueError(f'Client {i} has only {done} completed surface-frame callbacks')
        ready=re.search(rb'alpine-wayland: READY frames (\d+) size 128 96',trace)
        if not ready or int(ready.group(1))<3:
            raise ValueError(f'Client {i} did not reach configured final window state')
        completed.append(done)
    return completed

if __name__=='__main__':
    try:
        import argparse
        ap=argparse.ArgumentParser()
        ap.add_argument("log")
        ap.add_argument("--renderer",choices=["software","opengl"],default="software")
        a=ap.parse_args()
        counts=check(pathlib.Path(a.log).read_bytes(),a.renderer)
    except (ValueError,OSError) as e:raise SystemExit(str(e))
    print(f'PASS: {len(counts)} real Sway outputs, every 128x96 RGBA pixel verified; completed frame callbacks {counts}')
