#!/usr/bin/env python3
"""Bounded fresh physical capture; acknowledge only visible real Dolphin content."""
import argparse
import pathlib
import subprocess
import sys
import time

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('socket')
ap.add_argument('png')
args = ap.parse_args()
top = pathlib.Path(__file__).resolve().parent
deadline = time.monotonic() + 35
attempt = 0
while True:
    attempt += 1
    # qemu-screenshot removes old evidence first; no cached image may pass.
    capture = subprocess.run([sys.executable, str(top / 'qemu-screenshot.py'),
                              args.socket, args.png], capture_output=True, text=True,
                             timeout=25)
    if capture.returncode == 0:
        result = subprocess.run([sys.executable, str(top / 'desktop-visible-check.py'),
                                 args.png, '--text', args.png + '.ocr.txt'],
                                capture_output=True, text=True, timeout=16)
        if result.returncode == 0:
            print(result.stdout.strip())
            print(f'PASS: physical Dolphin presentation observed on fresh capture {attempt}')
            break
        detail = result.stderr.strip()
    else:
        detail = capture.stderr.strip()
    if time.monotonic() >= deadline:
        raise SystemExit(f'FAIL: Dolphin physical presentation deadline: {detail}')
    time.sleep(1)
