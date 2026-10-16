#!/usr/bin/env python3
"""Check real Linux headless comparison captures; NOT 9os/physical-input proof."""
import argparse
import base64
import hashlib
import importlib.machinery
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
load = importlib.machinery.SourceFileLoader
wallpaper = load('plasma_check', str(HERE / 'alpine-plasma-check.py')).load_module().check
visible = load('visible_check', str(HERE / 'desktop-visible-check.py')).load_module().check


def check(log, output):
    if log.stat().st_size > 64 * 1024 * 1024:
        raise ValueError('Linux comparison log exceeds bound')
    raw = log.read_bytes()
    for marker in (b'LINUX_ORACLE_KERNEL=Linux', b'LINUX_ORACLE_UID=1000',
                   b'KDE_INTERACTIVE_PTY', b'DOLPHIN_MODEL_READY uid=1000',
                   b'alpine-dolphin: PASSED real UID1000 directory listing, rename/create notifications and clean exit'):
        if marker not in raw:
            raise ValueError('Missing real comparison marker: ' + marker.decode())
    wallpaper(raw)  # The unchanged stock wallpaper/panel oracle.
    frames = re.findall(rb'LINUX_FRESH_PNG_BEGIN\r?\n(.*?)LINUX_FRESH_PNG_END', raw, re.S)
    if not frames or len(frames) > 100:
        raise ValueError('Missing or excessive fresh Linux captures')
    output.mkdir(parents=True, exist_ok=True)
    seen = set()
    for index, frame in enumerate(frames):
        if len(frame) > 10 * 1024 * 1024:
            raise ValueError('Oversized capture')
        data = base64.b64decode(re.sub(rb'\s', b'', frame), validate=True)
        digest = hashlib.sha256(data).digest()
        if digest in seen:
            continue
        seen.add(digest)
        path = output / ('linux-fresh-%d.png' % index)
        path.write_bytes(data)
        passed, text = visible(path)  # Same whole-token, panel-excluding OCR.
        path.with_suffix('.ocr.txt').write_text(text)
        if passed:
            return len(frames), path
    raise ValueError('Actual Linux Dolphin content missing/stale throughout fresh capture window')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        count, path = check(args.log, args.output or args.log.with_suffix('.captures'))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, 'FAIL: ' + str(error) + '\n')
    print('PASS: Linux headless wallpaper, Konsole PTY, actual Dolphin model/pixels and clean exit; %d fresh captures' % count)
    print('Evidence: ' + str(path))
    print('This is NOT 9os or physical keyboard acceptance.')


if __name__ == '__main__':
    main()
