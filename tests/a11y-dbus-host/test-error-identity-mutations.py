#!/usr/bin/env python3
"""Independent native semantic mutations of added diagnostics, not IPC proof."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1] / 'userland/tests/a11y-dbus/terminal-dbus.c'
MUTATIONS = [
    ('remote-misclassified-as-local-timeout', 'kind="remote-dbus";', 'kind="local-timeout";'),
    ('wrong-local-timeout-enum', 'error_matches(e,io_error_quark(),24)', 'error_matches(e,io_error_quark(),23)'),
    ('missing-bundle-not-unknown', 'const char *kind="unknown",*name_status="unknown";', 'const char *kind="other",*name_status="unknown";'),
    ('wrong-correlated-bus', 'bt=identity_hex(bh,r->name,256)', 'bt=identity_hex(bh,"org.wrong",256)'),
    ('uppercase-hex', '"0123456789abcdef"', '"0123456789ABCDEF"'),
    ('missing-truncation-flag', 'nt=identity_hex(nh,name?name:"",256)', 'nt=(identity_hex(nh,name?name:"",256),0)'),
    ('missing-sync-record', 'error_identity(e,method,r);', ''),
    ('missing-async-record', 'error_identity(e,p->method,p->ref);', ''),
    ('early-release-before-both-callbacks', 'while(!p[0].done||!p[1].done)', 'while(!p[0].done\u0026\u0026!p[1].done)'),
    ('missing-two-record-cap', 'if(error_identities>=2)return;', ''),
]


def run(source, log=None):
    result = subprocess.run([sys.executable, str(HERE / 'test-error-identity.py')],
                            env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1',
                                 'A11Y_ERROR_SOURCE': str(source)},
                            text=True, capture_output=True, timeout=30)
    text = result.stdout + result.stderr
    if log:
        log.write_text(text)
    return result, text


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--log-dir', type=Path)
    args = ap.parse_args()
    if args.log_dir:
        args.log_dir.mkdir(parents=True, exist_ok=True)
    original = SOURCE.read_text()
    baseline, text = run(SOURCE, args.log_dir / 'baseline.log' if args.log_dir else None)
    assert baseline.returncode == 0, text
    print('PASS unmodified native diagnostic controls', flush=True)
    with tempfile.TemporaryDirectory(prefix='a11y-error-mutations-') as temp:
        for name, old, new in MUTATIONS:
            assert original.count(old) == 1, (name, 'mutation selector drift')
            mutant = original.replace(old, new, 1)
            path = Path(temp) / (name + '.c')
            path.write_text(mutant)
            result, text = run(path, args.log_dir / (name + '.log') if args.log_dir else None)
            # Compilation/infrastructure failure is never a semantic kill.
            assert result.returncode == 1 and 'FAILED (failures=' in text, (name, text)
            assert 'ERROR:' not in text and 'CalledProcessError' not in text, (name, text)
            assert 'AssertionError' in text, (name, text)
            print('SEMANTIC MUTANT REJECTED', name, hashlib.sha256(mutant.encode()).hexdigest(), flush=True)
    print(f'{len(MUTATIONS)}/{len(MUTATIONS)} semantic diagnostic mutants rejected; synthetic native only')


if __name__ == '__main__':
    main()
