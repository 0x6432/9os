#!/usr/bin/env python3
"""Independent diagnostic/acceptance mutations; real synthetic IPC, NOT GUI proof."""
import argparse
from pathlib import Path
import shutil
import re
import subprocess
import sys
import tempfile

TOP = Path(__file__).resolve().parents[2]
SOURCE = Path('userland/tests/a11y-dbus/terminal-dbus.c')
TEST = Path('tests/a11y-dbus-host/test-native.py')
MOCK = Path('tests/a11y-dbus-host/mock-server.c')

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--signed-root', type=Path, default=TOP/'userland/alpine-root-x86_64')
    ap.add_argument('--log-dir', type=Path)
    args = ap.parse_args()
    source = (TOP/SOURCE).read_text()
    mutations = {
        'lost-focus-count': source.replace('focused_states+=(unsigned)focused;', 'focused_states+=0;', 1),
        'invalid-state-counted': source.replace('  valid_states++;', '').replace('  if(variant_count(a)!=2)', '  valid_states++;\n  if(variant_count(a)!=2)', 1),
        'hidden-protocol-error': source.replace('visible_states,models,failed);', 'visible_states,models,0);', 1),
        'cross-terminal-state-merge': source.replace('if(focused&&showing&&visible)models++;', 'if(focused_states&&showing_states&&visible_states)models++;', 1),
        'unauthenticated-application': source.replace('if(owner(&app))', 'if(1)', 1),
    }
    if args.log_dir:
        args.log_dir.mkdir(parents=True, exist_ok=True)
    for name, mutant in mutations.items():
        assert mutant != source, name
        with tempfile.TemporaryDirectory(prefix='9os-witness-mutant-') as tmp:
            root = Path(tmp)
            for rel in [SOURCE, TEST, MOCK]:
                (root/rel).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(TOP/rel, root/rel)
            (root/SOURCE).write_text(mutant)
            result = subprocess.run([sys.executable, str(root/TEST), '--signed-root', str(args.signed_root.resolve())],
                                    capture_output=True, text=True, timeout=45)
            text = result.stdout + result.stderr
            if args.log_dir:
                (args.log_dir/(name+'.out')).write_text(text)
            # A compiler/setup error is NOT a rejected semantic mutation.
            assert result.returncode == 1 and re.search(r'^AssertionError(?::|$)', text, re.M) is not None, (name, result.returncode, text[-3000:])
            print('PASS independently rejected witness mutation', name, flush=True)
    print('5/5 independent witness/acceptance mutations rejected; synthetic IPC only')

if __name__ == '__main__':
    main()
