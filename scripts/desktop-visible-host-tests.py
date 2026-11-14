#!/usr/bin/env python3
"""Text-matching controls only, NOT evidence that any guest GUI rendered."""
import importlib.util
import pathlib
import tempfile
from PIL import Image, ImageDraw
p = pathlib.Path(__file__).with_name('desktop-visible-check.py')
spec = importlib.util.spec_from_file_location('visible', p)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
cases = [
    ('files — Dolphin\nafter.txt\nnew.txt\n2 files (64 B)', True),
    ('Dolphin after.txt new.txt', True),
    ('after.txt new.txt', False),
    ('', False),
    ('files — Dolphin', False),
    ('after.txt', False),
    ('new.txt', False),
    ('Dolphin before.txt after.txt new.txt', False),
    ('Dolphin /tmp/after.txt /tmp/new.txt', False),
    ('Dolphin notafter.txt new.txt', False),
    ('Dolphin after.txt new.txt.bak', False),
]
for text, expected in cases:
    assert m.visible_files(text) == expected, (text, expected)
with tempfile.TemporaryDirectory() as d:
    for name in ('blank', 'panel-only', 'small'):
        im = Image.new('RGB', (320, 200) if name == 'small' else (800, 600))
        if name == 'panel-only':
            ImageDraw.Draw(im).text((5, 565), 'Dolphin after.txt new.txt', fill='white')
        image = pathlib.Path(d) / (name + '.png')
        im.save(image)
        try:
            passed, text = m.check(image)
            assert not passed, (name, text)
        except ValueError:
            assert name == 'small'
print(f'PASS: {len(cases)} text and 3 image visibility controls (not GUI evidence)')
