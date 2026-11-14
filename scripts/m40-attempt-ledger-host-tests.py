#!/usr/bin/env python3
"""Inert host controls for the reservation primitive, never launches a guest."""
import concurrent.futures
from importlib.machinery import SourceFileLoader
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/m40-attempt-ledger.py'
LEDGER = SourceFileLoader('m40_attempt_ledger', str(SCRIPT)).load_module()
DIGEST = 'a' * 64


class ReservationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='m40-ledger-host-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.dir = self.root / 'private'
        self.dir.mkdir(mode=0o700)

    def cli(self, directory=None, digest=DIGEST):
        return subprocess.run(['python3', str(SCRIPT), str(directory or self.dir),
                               '--contract-sha256', digest], capture_output=True, text=True, timeout=10)

    def test_reserves_once_and_preserves_record(self):
        self.assertEqual(self.cli().returncode, 0)
        path = self.dir / LEDGER.NAME
        data = path.read_bytes()
        self.assertEqual(json.loads(data), {'version': 1, 'contract_sha256': DIGEST})
        self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        again = self.cli(digest='b' * 64)
        self.assertNotEqual(again.returncode, 0)
        self.assertIn('already reserved', again.stderr)
        self.assertEqual(path.read_bytes(), data)

    def test_simultaneous_reservations_only_one_succeeds(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            results = list(pool.map(lambda _: self.cli(), range(24)))
        self.assertEqual(sum(r.returncode == 0 for r in results), 1)
        self.assertTrue(all('already reserved' in r.stderr for r in results if r.returncode))
        self.assertEqual(json.loads((self.dir / LEDGER.NAME).read_text())['contract_sha256'], DIGEST)

    def test_preexisting_marker_refused_without_following_link(self):
        target = self.root / 'untouched'; target.write_text('safe')
        (self.dir / LEDGER.NAME).symlink_to(target)
        self.assertNotEqual(self.cli().returncode, 0)
        self.assertEqual(target.read_text(), 'safe')

    def test_refuses_unsafe_paths_modes_and_digests(self):
        link = self.root / 'link'; link.symlink_to(self.dir, target_is_directory=True)
        child_link = self.root / 'indirect'; child_link.symlink_to(self.root, target_is_directory=True)
        for path in (link, child_link / 'private', Path('/'), Path('.')):
            self.assertNotEqual(self.cli(directory=path).returncode, 0, path)
        for suffix in ('//private', '/./private', '/private/..', '/private/'):
            self.assertNotEqual(self.cli(directory=str(self.root) + suffix).returncode, 0, suffix)
        self.dir.chmod(0o755)
        self.assertNotEqual(self.cli().returncode, 0)
        self.dir.chmod(0o700)
        for value in ('', 'A'*64, 'g'*64, 'a'*63, 'a'*65):
            self.assertNotEqual(self.cli(digest=value).returncode, 0)
        self.assertFalse((self.dir / LEDGER.NAME).exists())

    def test_fsync_failure_retains_marker_and_blocks_retry(self):
        real_fsync = os.fsync
        # One file fsync plus one for each open directory from leaf to root.
        for fail_at in range(1, len(self.dir.parts) + 2):
            with self.subTest(fail_at=fail_at):
                self.dir = self.root / ('private' + str(fail_at)); self.dir.mkdir(mode=0o700)
                calls = []
                def fsync(fd):
                    calls.append(fd)
                    if len(calls) == fail_at: raise OSError('injected fsync failure')
                    return real_fsync(fd)
                with mock.patch.object(LEDGER.os, 'fsync', side_effect=fsync):
                    with self.assertRaises(OSError): LEDGER.reserve(str(self.dir), DIGEST)
                self.assertTrue((self.dir / LEDGER.NAME).exists())
                self.assertNotEqual(self.cli().returncode, 0)

    def test_syncs_record_then_ledger_and_ancestors_bottom_up(self):
        real_fsync = os.fsync; seen=[]
        def observe(fd):
            seen.append(os.readlink('/proc/self/fd/'+str(fd)))
            return real_fsync(fd)
        with mock.patch.object(LEDGER.os, 'fsync', side_effect=observe):
            LEDGER.reserve(str(self.dir), DIGEST)
        expected=[str(self.dir/LEDGER.NAME),str(self.dir)] + [str(p) for p in self.dir.parents]
        self.assertEqual(seen, expected)

    def test_empty_partial_and_directory_markers_all_consume_reservation(self):
        for kind in ('empty','partial','directory'):
            with self.subTest(kind=kind):
                d=self.root/('case-'+kind);d.mkdir(mode=0o700);p=d/LEDGER.NAME
                if kind=='directory': p.mkdir()
                else: p.write_text('' if kind=='empty' else '{"version":')
                self.assertNotEqual(self.cli(directory=d).returncode,0)
                self.assertTrue(p.exists())

    def test_create_failure_does_not_create_marker(self):
        real_open = os.open
        def fail(name, *args, **kwargs):
            if name == LEDGER.NAME: raise OSError('injected create failure')
            return real_open(name, *args, **kwargs)
        with mock.patch.object(LEDGER.os, 'open', side_effect=fail):
            with self.assertRaises(OSError): LEDGER.reserve(str(self.dir), DIGEST)
        self.assertFalse((self.dir / LEDGER.NAME).exists())
        # Production must reconcile even an absent marker after a failure.

    def test_partial_write_failure_retains_marker(self):
        real_write=os.write; calls=[]
        def fail(fd, data):
            calls.append(fd)
            if len(calls)==1: return real_write(fd, data[:4])
            raise OSError('injected partial-write failure')
        with mock.patch.object(LEDGER.os, 'write', side_effect=fail):
            with self.assertRaises(OSError): LEDGER.reserve(str(self.dir), DIGEST)
        self.assertEqual((self.dir / LEDGER.NAME).read_bytes(), b'{"co')
        self.assertNotEqual(self.cli().returncode,0)

    def test_process_death_leaves_visible_marker_in_host_control(self):
        # Abrupt process exit is not a simulation of power-loss recovery.
        code = """import os,sys
from importlib.machinery import SourceFileLoader
m=SourceFileLoader('ledger',sys.argv[1]).load_module()
real=os.fsync; count=[0]
def die(fd):
 count[0]+=1
 if count[0]==int(sys.argv[3]): os._exit(73)
 real(fd)
m.os.fsync=die
m.reserve(sys.argv[2],sys.argv[4])
"""
        for die_at in (1,2):
            with self.subTest(die_at=die_at):
                d=self.root/('crash-'+str(die_at)); d.mkdir(mode=0o700)
                r=subprocess.run(['python3','-c',code,str(SCRIPT),str(d),str(die_at),DIGEST],timeout=10)
                self.assertEqual(r.returncode,73)
                self.assertTrue((d/LEDGER.NAME).exists())
                self.assertNotEqual(self.cli(directory=d).returncode,0)

    def test_short_writes_are_completed(self):
        real_write = os.write
        def short(fd, data): return real_write(fd, data[:min(len(data), 5)])
        with mock.patch.object(LEDGER.os, 'write', side_effect=short):
            LEDGER.reserve(str(self.dir), DIGEST)
        self.assertEqual(json.loads((self.dir / LEDGER.NAME).read_bytes())['contract_sha256'], DIGEST)

    def test_zero_write_is_a_failure_and_consumes_marker(self):
        with mock.patch.object(LEDGER.os, 'write', return_value=0):
            with self.assertRaises(OSError): LEDGER.reserve(str(self.dir), DIGEST)
        self.assertTrue((self.dir / LEDGER.NAME).exists())
        self.assertNotEqual(self.cli().returncode, 0)


if __name__ == '__main__': unittest.main()
