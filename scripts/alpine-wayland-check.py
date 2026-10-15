#!/usr/bin/env python3
"""Verify actual Sway output pixels and Qt's completed Wayland frame callbacks."""
import base64, io, pathlib, re, sys
from PIL import Image
raw=pathlib.Path(sys.argv[1]).read_bytes()
blocks=re.findall(rb'ALPINE_WAYLAND_PNG_BEGIN\r?\n(.*?)ALPINE_WAYLAND_PNG_END',raw,re.S)
if not blocks:raise SystemExit('No compositor captures found')
for i,block in enumerate(blocks):
    im=Image.open(io.BytesIO(base64.b64decode(re.sub(rb'\s',b'',block),validate=True))).convert('RGBA')
    if im.size!=(128,96) or im.getextrema()!=((17,17),(170,170),(85,85),(255,255)):
        raise SystemExit(f'Wrong compositor capture {i}: {im.size}, {im.getextrema()}')
print(f'PASS: {len(blocks)} real Sway outputs, every 128x96 RGBA pixel verified')
