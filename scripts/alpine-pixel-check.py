#!/usr/bin/env python3
"""Verify every selected Qt Quick capture in a flushed QEMU console log."""
import argparse, base64, io, pathlib, re
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument("log")
ap.add_argument("--prefix", choices=["ALPINE_QUICK", "SOURCE_QUICK"], default="ALPINE_QUICK")
ap.add_argument("--expected-count", type=int)
args = ap.parse_args()
from PIL import Image
raw = pathlib.Path(args.log).read_bytes()
prefix = args.prefix.encode()
blocks = re.findall(prefix + rb'_PNG_BEGIN\r?\n(.*?)' + prefix + rb'_PNG_END', raw, re.S)
if raw.count(prefix + b'_PNG_BEGIN') != len(blocks) or raw.count(prefix + b'_PNG_END') != len(blocks):
    raise SystemExit('Incomplete or malformed guest Qt Quick capture')
if args.expected_count is not None and (args.expected_count < 1 or len(blocks) != args.expected_count):
    raise SystemExit(f'Expected {args.expected_count} captures, found {len(blocks)}')
if not blocks:
    raise SystemExit('No guest Qt Quick captures found')
for i, block in enumerate(blocks):
    image = Image.open(io.BytesIO(base64.b64decode(re.sub(rb'\s', b'', block), validate=True))).convert('RGBA')
    if image.size != (96, 64) or image.getextrema() != ((204,204), (51,51), (17,17), (255,255)):
        raise SystemExit(f'Capture {i} has wrong size/pixels: {image.size}, {image.getextrema()}')
print(f'PASS: {len(blocks)} guest-rendered 96x64 images, every RGBA pixel verified')
