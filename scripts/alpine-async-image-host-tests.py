#!/usr/bin/env python3
"""Pixel comparison controls only; generated inputs are NOT guest GUI evidence."""
import importlib.util
import pathlib
from PIL import Image, ImageOps
p = pathlib.Path(__file__).with_name('alpine-async-image-check.py')
spec = importlib.util.spec_from_file_location('imagecheck', p)
m = importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
source = Image.new('RGB', (640, 480))
source.putdata([(x % 256, y % 256, (x + 2 * y) % 256) for y in range(480) for x in range(640)])
expected = ImageOps.fit(source, (320, 240), method=Image.Resampling.BILINEAR)
m.check_image(expected, source)
for wrong in (Image.new('RGB', (320, 240)), Image.new('RGB', (800, 600)),
              ImageOps.mirror(expected), Image.new('RGBA', (320, 240), (0, 0, 0, 0))):
    try:m.check_image(wrong, source)
    except ValueError:pass
    else:raise AssertionError('invalid image accepted')
print('PASS: async-image comparison positive and four negative controls (not GUI evidence)')
