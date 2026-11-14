#!/usr/bin/env python3
"""Historical 07fd-only evidence-gap control, not a future acceptance test."""
from pathlib import Path
import hashlib
import importlib.util
import json
import os
import sys
p = Path(__file__).resolve().parents[1] / 'test-readbacks.py'
spec = importlib.util.spec_from_file_location('original_readbacks', p)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
print('New native command-mock baseline, NOT original PR42 guest evidence.')
print('consumer sha256=' + hashlib.sha256(m.PROBE.read_bytes()).hexdigest())
print('checker sha256=' + hashlib.sha256(m.CHECK.read_bytes()).hexdigest())
r, calls = m.ReadbackTests().run_probe(m.good_config())
print('exit=' + str(r.returncode))
print('stdout=' + repr(r.stdout))
print('stderr=' + repr(r.stderr))
print('ordered calls=' + json.dumps(calls))
assert r.returncode == 0 and calls == m.CALLS
assert r.stdout == 'alpine-udisks: real disk/partition/root filesystem enumerated uid=1000\n'
print('CONFIRMED GAP: all three typed Gets required, but no individual reply bytes emitted.')
