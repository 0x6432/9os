#!/usr/bin/env python3
"""Verify every prebuilt Qt Quick capture in a flushed QEMU console log."""
import base64, io, pathlib, re, sys
from PIL import Image
raw = pathlib.Path(sys.argv[1]).read_bytes()
blocks = re.findall(rb'ALPINE_QUICK_PNG_BEGIN\r?\n(.*?)ALPINE_QUICK_PNG_END', raw, re.S)
if not blocks:
    raise SystemExit('No guest Qt Quick captures found')
for i, block in enumerate(blocks):
    image = Image.open(io.BytesIO(base64.b64decode(re.sub(rb'\s', b'', block), validate=True))).convert('RGBA')
    if image.size != (96, 64) or image.getextrema() != ((204,204), (51,51), (17,17), (255,255)):
        raise SystemExit(f'Capture {i} has wrong size/pixels: {image.size}, {image.getextrema()}')
print(f'PASS: {len(blocks)} guest-rendered 96x64 images, every RGBA pixel verified')
