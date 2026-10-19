#!/usr/bin/env python3
"""Require explicit assertion mismatches for diagnostic/acceptance regressions.

All mutants are temporary in-memory/Python/shell fixtures, never runtime edits.
Syntax/import/runtime errors are NOT accepted as mutation rejection evidence.
"""
from pathlib import Path
import hashlib
import importlib.util
import io
import re
import tempfile
import types
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('evidence_mutation_controls', HERE / 'test-evidence.py')
T = importlib.util.module_from_spec(spec)
spec.loader.exec_module(T)
OLD = T.OLD


def fields(line):
    return dict(part.split('=', 1) for part in line.split()[1:])


def helper(source):
    module = types.ModuleType('temporary_evidence_mutant')
    exec(compile(source, '<temporary-evidence-mutant>', 'exec'), module.__dict__)
    return module


def verify_raw_safe(module):
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'reply'; path.write_bytes(b'FAIL\nraw response')
        line = module.record('1', '0', '1', path)
        assert re.search(rb'\bFAIL', line.encode()) is None, 'raw bytes reached host sentinel'


def verify_capture_cap(module):
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'reply'; path.write_bytes(b'x' * 1200)
        f = fields(module.record('1', '0', '1', path))
        assert f['captured_bytes'] == '1024', 'capture exceeded 1024-byte contract'
        assert f['truncated'] == '1' and f['complete'] == '0', 'oversize promoted complete'


def verify_failed_unknown(module):
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'reply'; path.write_bytes(b'plausible partial reply')
        f = fields(module.record('1', '1', 'unknown', path))
        assert f['command_status'] == '1', 'command status falsified'
        assert f['complete'] == 'unknown' and f['unknown'] == '1', 'failed response promoted complete'


def verify_empty_unknown(module):
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'reply'; path.write_bytes(b'')
        f = fields(module.record('1', '0', '1', path))
        assert f['capture'] == 'empty' and f['complete'] == '0', 'empty file promoted full reply'


def verify_bounded_read(module):
    class Reader(io.BytesIO):
        def read(self, size=-1):
            assert size == 1025, 'unbounded or oversized evidence read'
            return super().read(size)
    with patch('builtins.open', return_value=Reader(b'x' * 2000)):
        module.record('1', '0', '1', 'fixture')


def verify_shell_command_failure(source):
    config = OLD.good_config(); config['replies'][1]['status'] = 124
    result, calls = OLD.ReadbackTests().run_probe(config, probe_text=source)
    assert result.returncode == 124, 'original command failure swallowed'
    assert len(calls) == 2, 'queries continued after command failure'


def verify_shell_checker_failure(source):
    config = OLD.good_config(); config['replies'][1]['stdout'] = T.OLD.device_reply('/dev/wrong')
    result, calls = OLD.ReadbackTests().run_probe(config, probe_text=source)
    assert result.returncode == 1, 'original checker failure swallowed'
    assert len(calls) == 2, 'queries continued after checker failure'


def verify_shell_recorder_non_authoritative(source):
    with tempfile.TemporaryDirectory() as tmp:
        recorder = Path(tmp) / 'error.py'; recorder.write_text('raise SystemExit(9)\n')
        result, calls = OLD.ReadbackTests().run_probe(OLD.good_config(), recorder=recorder, probe_text=source)
    assert result.returncode == 0, 'recording vetoed original success'
    assert calls == OLD.CALLS, 'recording changed original query sequence'


def verify_shell_recorder_stderr_safe(source):
    with tempfile.TemporaryDirectory() as tmp:
        recorder = Path(tmp) / 'error.py'
        recorder.write_text('import sys\nsys.stderr.write("FAIL arbitrary recorder detail\\n")\nraise SystemExit(9)\n')
        result, calls = OLD.ReadbackTests().run_probe(OLD.good_config(), recorder=recorder, probe_text=source)
    assert result.returncode == 0, 'recording changed original success'
    assert not re.search(rb'\bFAIL', (result.stdout + result.stderr).encode()), 'recorder stderr reached host sentinel'


def rejected(name, normal, mutant, verifier):
    verifier(normal)
    try:
        verifier(mutant)
    except AssertionError as error:
        print('rejected ' + name + ': ' + str(error) +
              '; mutant_sha256=' + hashlib.sha256(mutant.encode()).hexdigest())
    else:
        raise AssertionError('surviving mutation: ' + name)


def main():
    source = OLD.EVIDENCE.read_text()
    mutations = [
        ('raw-instead-of-hex', 'prefix.hex()', 'prefix.decode("ascii")', verify_raw_safe),
        ('double-capture-cap', 'MAX_CAPTURE = 1024', 'MAX_CAPTURE = 2048', verify_capture_cap),
        ('failed-eof-is-complete', "if command_status != '0':", 'if False:', verify_failed_unknown),
        ('empty-is-complete', "'empty', '0', '0'", "'complete', '1', '0'", verify_empty_unknown),
        ('unbounded-read', 'stream.read(MAX_CAPTURE + 1)', 'stream.read()', verify_bounded_read),
    ]
    print('helper_source_sha256=' + hashlib.sha256(source.encode()).hexdigest())
    for name, anchor, replacement, verifier in mutations:
        assert source.count(anchor) == 1, name
        mutant = source.replace(anchor, replacement)
        # Explicit assertion failures only; malformed syntax/imports cannot pass.
        verifier(helper(source))
        try:
            verifier(helper(mutant))
        except AssertionError as error:
            print('rejected ' + name + ': ' + str(error) +
                  '; mutant_sha256=' + hashlib.sha256(mutant.encode()).hexdigest())
        else:
            raise AssertionError('surviving mutation: ' + name)
    source = OLD.PROBE.read_text()
    print('consumer_source_sha256=' + hashlib.sha256(source.encode()).hexdigest())
    shell_mutations = [
        ('swallow-command-error', 'return "$property_command_rc"', 'return 0', verify_shell_command_failure),
        ('swallow-checker-error', 'return "$property_checker_rc"', 'return 0', verify_shell_checker_failure),
        ('recorder-veto-success', '"$property_checker_rc" || true', '"$property_checker_rc" || true; return 1',
         verify_shell_recorder_non_authoritative),
        ('recorder-stderr-leak', '"$property_reply" 2>/dev/null;then :;else', '"$property_reply";then :;else',
         verify_shell_recorder_stderr_safe),
    ]
    for name, anchor, replacement, verifier in shell_mutations:
        assert source.count(anchor) == 1, name
        rejected(name, source, source.replace(anchor, replacement), verifier)
    print('9/9 mutations rejected by explicit assertion mismatches; no guest/model proof.')


if __name__ == '__main__':
    main()
