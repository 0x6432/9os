#!/usr/bin/env python3
"""Require rendered Dolphin filenames in a real physical-display screenshot.

This supplements, never replaces, exact-PID AT-SPI model and physical PTY checks.
OCR is deliberately conservative: inconclusive text is a failure, not a pass.
"""
import argparse
import pathlib
import re
import subprocess
import tempfile
from PIL import Image


def visible_files(text):
    # Whole OCR tokens only; never accept path substrings or prefix/suffix hits.
    words = set(re.findall(r'\S+', text))
    return {'Dolphin', 'after.txt', 'new.txt'} <= words and 'before.txt' not in words


def check(path):
    with Image.open(path) as im:
        im.load()
        if im.width < 640 or im.height < 480:
            raise ValueError('physical display too small')
        # Exclude the taskbar: a launch icon/title cannot stand in for content.
        im = im.convert('RGB').crop((0, 0, im.width, im.height - 70))
        im = im.resize((im.width * 2, im.height * 2))
        with tempfile.TemporaryDirectory(prefix='9os-visible-') as d:
            crop = pathlib.Path(d) / 'content.png'
            im.save(crop)
            r = subprocess.run(['tesseract', str(crop), 'stdout', '-l', 'eng',
                                '--psm', '11'], capture_output=True, text=True,
                               timeout=12, check=True)
    return visible_files(r.stdout), r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('png')
    ap.add_argument('--text', help='save unmodified OCR output for diagnosis')
    args = ap.parse_args()
    try:
        passed, text = check(args.png)
        if args.text:
            pathlib.Path(args.text).write_text(text)
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        ap.exit(1, f'FAIL: physical Dolphin visibility could not be checked: {e}\n')
    if not passed:
        ap.exit(1, 'FAIL: physical Dolphin content missing, stale or unreadable\n')
    print('PASS: actual physical display shows Dolphin with after.txt and new.txt, not before.txt')


if __name__ == '__main__':
    main()
