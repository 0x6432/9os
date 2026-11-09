#!/usr/bin/env python3
"""Bounded fresh physical capture; acknowledge only visible real Dolphin content."""
import argparse
import os
import pathlib
import subprocess
import sys
import tempfile
import time

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('socket')
ap.add_argument('png')
args = ap.parse_args()
top = pathlib.Path(__file__).resolve().parent
output = pathlib.Path(os.path.abspath(args.png))
ocr_output = pathlib.Path(str(output) + '.ocr.txt')
if output.exists() or output.is_symlink() or ocr_output.exists() or ocr_output.is_symlink():
    raise SystemExit('FAIL: Dolphin capture destinations must be fresh')
deadline = time.monotonic() + 35
attempt = 0
# A distinct private candidate per try avoids deleting or overwriting evidence.
with tempfile.TemporaryDirectory(prefix='.9os-dolphin-', dir=output.parent) as directory:
    while True:
        attempt += 1
        candidate = pathlib.Path(directory) / (str(attempt) + '.png')
        ocr_candidate = pathlib.Path(str(candidate) + '.ocr.txt')
        capture = subprocess.run([sys.executable, str(top / 'qemu-screenshot.py'),
                                  args.socket, str(candidate)], capture_output=True, text=True,
                                 timeout=25)
        if capture.returncode == 0:
            result = subprocess.run([sys.executable, str(top / 'desktop-visible-check.py'),
                                     str(candidate), '--text', str(ocr_candidate)],
                                    capture_output=True, text=True, timeout=16)
            if result.returncode == 0:
                # link() fails if either final pathname exists, including symlinks.
                # A failed second link can leave the first output: report failure,
                # retain it, and never overwrite it silently.
                os.link(candidate, output, follow_symlinks=False)
                os.link(ocr_candidate, ocr_output, follow_symlinks=False)
                print(result.stdout.strip())
                print(f'PASS: physical Dolphin presentation observed on fresh capture {attempt}')
                break
            detail = result.stderr.strip()
        else:
            detail = capture.stderr.strip()
        if time.monotonic() >= deadline:
            raise SystemExit(f'FAIL: Dolphin physical presentation deadline: {detail}')
        time.sleep(1)
