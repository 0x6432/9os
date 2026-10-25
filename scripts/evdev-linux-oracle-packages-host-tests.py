#!/usr/bin/env python3
"""Native synthetic APK/runner controls. No VM/compiler/network is used.

Synthetic unsigned tar fixtures exercise orchestration with a fake APK runner;
these tests are NOT a replacement for real native APK signature/assembly proof.
The frozen legacy script reproduces the original solver failure before QEMU.
"""
import copy
import gzip
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import tarfile
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / 'scripts/evdev-linux-oracle-packages.py'
LOCK = ROOT / 'userland/ports/linux-oracle-lock.json'
BEFORE_SHA = '62911e9b2b9e62e1e90ada1871f016551345e028ce571fe904a0eb0e919cfbce'

LEGACY = '#!/bin/sh\n# Boot the exact tiny guest consumer on a signed PREBUILT Alpine Linux kernel.\n# x86_64 only. No Qt/Mesa/LLVM/libc build, host input device or host root needed.\nset -eu\ncd "$(dirname "$0")/.."\n[ "$(uname -m)" = x86_64 ] || { echo \'Linux input oracle requires x86_64 host\' >&2;exit 2; }\nS=$(cd userland/alpine-root-x86_64 && pwd -P)\n[ -x "$S/usr/bin/evdevframetest" ] || { echo \'Run alpine-build-tests.sh x86_64 first\' >&2;exit 2; }\nL=${1:-build/evdev-linux-oracle.log}\nmkdir -p "$(dirname "$L")"\nD=$(mktemp -d)\ntrap \'rm -rf "$D"\' EXIT HUP INT TERM\nmkdir -p "$D/apk" "$D/init"/bin "$D/init"/lib "$D/init"/dev "$D/init"/proc "$D/init"/sys "$D/init"/modules\n# Exact reviewed version; fail rather than silently testing another kernel.\n"$S/lib/ld-musl-x86_64.so.1" --library-path "$S/lib:$S/usr/lib" "$S/sbin/apk" \\\n --arch x86_64 --root "$D/apk" --initdb --no-scripts --no-chown --no-cache \\\n --keys-dir "$S/etc/apk/keys" --repository https://dl-cdn.alpinelinux.org/alpine/v3.22/main \\\n add linux-virt=6.12.113-r0 > "$L.packages.log" 2>&1\ncp "$S/bin/busybox" "$D/init/bin/"\ncp "$S/usr/bin/evdevframetest" "$D/init/bin/"\ncp -L "$S/lib/ld-musl-x86_64.so.1" "$D/init/lib/ld-musl-x86_64.so.1"\nfor n in sh mount insmod poweroff;do ln -s busybox "$D/init/bin/$n";done\nfor n in evdev misc/uinput;do gzip -dc "$D/apk/lib/modules/6.12.113-0-virt/kernel/drivers/input/$n.ko.gz" > "$D/init/modules/$(basename "$n").ko";done\ncat > "$D/init/init" <<\'INIT\'\n#!/bin/sh\nexport PATH=/bin\nmount -t proc proc /proc\nmount -t sysfs sysfs /sys\nmount -t devtmpfs devtmpfs /dev\ninsmod /modules/evdev.ko\ninsmod /modules/uinput.ko\n/bin/evdevframetest\nrc=$?\necho LINUX_INPUT_ORACLE_RC=$rc\npoweroff -f\nINIT\nchmod +x "$D/init/init"\n(cd "$D/init" && find . -print0 | cpio -0 -o -H newc --quiet | gzip -1 > "$D/init.cpio.gz")\nsha256sum "$D/apk/boot/vmlinuz-virt" "$D/init/bin/evdevframetest" "$D/init.cpio.gz" > "$L.fixtures.sha256"\ntimeout 90 qemu-system-x86_64 -machine q35,accel=tcg -cpu max -smp 2 -m 512M \\\n -kernel "$D/apk/boot/vmlinuz-virt" -initrd "$D/init.cpio.gz" \\\n -append \'console=ttyS0 panic=-1 rdinit=/init\' -display none -serial stdio -no-reboot > "$L.raw" 2>&1\ntr -d \'\\r\' < "$L.raw" > "$L"\ngrep -E \'^(evdevframetest:|LINUX_INPUT_ORACLE_RC=)\' "$L"\ngrep -qx LINUX_INPUT_ORACLE_RC=0 "$L"\n'


def module(source=None):
    # Compilation/import setup is deliberately OUTSIDE any mutant assertion catch.
    m = types.ModuleType('oracle_contract_fixture')
    m.__file__ = str(HELPER)
    exec(compile(source or HELPER.read_text(), str(HELPER), 'exec'), m.__dict__)
    return m


class LegacyBaseline(unittest.TestCase):
    def test_exact_original_solver_failure_prevents_qemu(self):
        self.assertEqual(hashlib.sha256(LEGACY.encode()).hexdigest(), BEFORE_SHA)
        with tempfile.TemporaryDirectory(prefix='old-linux-reference-') as tmp:
            root = Path(tmp)
            (root / 'scripts').mkdir()
            (root / 'scripts/oracle.sh').write_text(LEGACY)
            s = root / 'userland/alpine-root-x86_64'
            for path in ['usr/bin', 'lib', 'sbin']:
                (s / path).mkdir(parents=True, exist_ok=True)
            (s / 'usr/bin/evdevframetest').write_text('#!/bin/sh\nexit 0\n')
            (s / 'usr/bin/evdevframetest').chmod(0o755)
            loader = s / 'lib/ld-musl-x86_64.so.1'
            loader.write_text('#!/bin/sh\nprintf "%s\\n" "$@" >"$EVENT"\n'
                              'printf "%s\\n" "ERROR: unable to select packages:" '
                              '"  linux-virt-6.12.114-r0:" "    breaks: world[linux-virt=6.12.113-r0]" >&2\nexit 1\n')
            loader.chmod(0o755)
            (root / 'bin').mkdir()
            qemu = root / 'bin/qemu-system-x86_64'
            qemu.write_text('#!/bin/sh\ntouch "$QEMU_CALLED"\nexit 99\n')
            qemu.chmod(0o755)
            result = subprocess.run(['/bin/sh', str(root / 'scripts/oracle.sh')], cwd=root,
                                    env={**os.environ, 'PATH': str(root / 'bin') + ':' + os.environ['PATH'],
                                         'EVENT': str(root / 'args'), 'QEMU_CALLED': str(root / 'qemu-called')},
                                    capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 1, result.stderr)
            args = (root / 'args').read_text().splitlines()
            self.assertEqual(args[-2:], ['add', 'linux-virt=6.12.113-r0'])
            self.assertEqual(args[args.index('--repository') + 1], 'https://dl-cdn.alpinelinux.org/alpine/v3.22/main')
            self.assertIn('--no-scripts', args)
            self.assertFalse((root / 'qemu-called').exists())
            self.assertFalse((root / 'build/evdev-linux-oracle.log.fixtures.sha256').exists())
            self.assertIn('breaks: world[linux-virt=6.12.113-r0]', (root / 'build/evdev-linux-oracle.log.packages.log').read_text())
            print('Frozen legacy oracle:', BEFORE_SHA, 'solver rc1; no fixture hash/QEMU', flush=True)


class Fixture:
    def __init__(self, m):
        self.m = m
        self.tmp = tempfile.TemporaryDirectory(prefix='linux-oracle-contract-')
        self.root = Path(self.tmp.name)
        self.cache = self.root / 'cache'
        self.target = self.root / 'target'
        self.cache.mkdir()
        self.target.mkdir()
        self.lock = json.loads(LOCK.read_text())
        self.lockpath = self.root / 'lock.json'
        self.contents = {}
        for item in self.lock['kernel']['contents']:
            if item['path'].endswith('.ko.gz'):
                raw = bytearray(64)
                raw[:6] = b'\x7fELF\x02\x01'
                struct.pack_into('<H', raw, 18, 62)
                raw = bytes(raw) + b'\x00vermagic=' + item['vermagic'].encode() + b'\x00'
                data = gzip.compress(raw, mtime=0)
                item.update(uncompressed_bytes=len(raw), uncompressed_sha256=hashlib.sha256(raw).hexdigest())
            else:
                data = b'synthetic kernel - NOT executable guest proof\n'
            self.contents[item['path']] = data
            item.update(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        for entry in self.lock['packages']:
            self.package(entry)
        self.save()
        self.calls = []
        self.fail = None
        self.bad_contents = False
        self.bad_dry = False
        self.bad_inventory = False
        self.prefix = ['fake-native-apk', '--keys-dir', '/private/pinned/keys',
                       '--repositories-file', '/dev/null', '--no-network']

    def save(self):
        self.lockpath.write_text(json.dumps(self.lock))

    def package(self, entry, change=None, duplicate=False):
        fields = [('pkgname', entry['name']), ('pkgver', entry['version']), ('arch', entry['arch'])]
        fields += [('depend', d) for d in entry['dependencies']]
        fields += [('provides', d) for d in entry['provides']]
        if change:
            fields = [(k, change[1] if k == change[0] else v) for k, v in fields]
        raw = ''.join(k + ' = ' + v + '\n' for k, v in fields).encode()
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode='w:gz') as tar:
            for _ in range(2 if duplicate else 1):
                info = tarfile.TarInfo('.PKGINFO')
                info.size = len(raw)
                tar.addfile(info, io.BytesIO(raw))
        data = stream.getvalue()
        (self.cache / entry['url'].rsplit('/', 1)[-1]).write_bytes(data)
        entry.update(bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), pkginfo_sha256=hashlib.sha256(raw).hexdigest())

    def runner(self, command):
        self.calls.append(command.copy())
        if 'verify' in command and self.fail == 'verify':
            raise subprocess.CalledProcessError(7, command)
        if 'add' in command and '--simulate' in command and self.bad_dry:
            dry = Path(command[command.index('--root') + 1])
            (dry / 'unexpected-empty-package-file').write_bytes(b'')
        if 'add' in command and '--simulate' in command and self.fail == 'dry':
            raise subprocess.CalledProcessError(2, command)
        if 'add' in command and '--simulate' not in command:
            if self.fail == 'add':
                raise subprocess.CalledProcessError(9, command)
            target = Path(command[command.index('--root') + 1])
            (target / 'lib/apk/db').mkdir(parents=True, exist_ok=True)
            entries = self.lock['packages'][:-1] if self.bad_inventory else self.lock['packages']
            (target / 'lib/apk/db/installed').write_text(''.join('P:' + e['name'] + '\nV:' + e['version'] + '\n\n' for e in entries))
            for name, data in self.contents.items():
                f = target / name
                f.parent.mkdir(parents=True, exist_ok=True)
                f.write_bytes(data + (b'broken' if self.bad_contents else b''))
        return subprocess.CompletedProcess(command, 0, stdout='', stderr='')

    def run(self, bypass_download=False):
        self.save()
        with patch.object(self.m, 'bootstrap', return_value=self.prefix):
            if bypass_download:
                with patch.object(self.m.BUILD, 'download', return_value=None):
                    return self.m.assemble(self.lockpath, self.cache, self.target, True, self.runner)
            return self.m.assemble(self.lockpath, self.cache, self.target, True, self.runner)

    def actual_adds(self):
        return [c for c in self.calls if 'add' in c and '--simulate' not in c]

    def protocol(self, case):
        verifies = [c for c in self.calls if 'verify' in c]
        case.assertEqual(len(verifies), 1)
        case.assertEqual(len(verifies[0][verifies[0].index('verify') + 1:]), 25)
        adds = [c for c in self.calls if 'add' in c]
        case.assertEqual(len(adds), 2)
        case.assertLess(self.calls.index(verifies[0]), self.calls.index(adds[0]))
        case.assertEqual(len(self.actual_adds()), 1)
        for c in self.calls:
            case.assertIn('--no-network', c)
            case.assertEqual(c[c.index('--repositories-file') + 1], '/dev/null')
            case.assertNotIn('--allow-untrusted', c)
            case.assertNotIn('--no-deps', c)
        for c in adds:
            for flag in ['--initdb', '--no-scripts', '--no-chown', 'linux-virt=6.12.114-r0']:
                case.assertIn(flag, c)
            case.assertEqual(len([v for v in c if v.endswith('.apk')]), 25)
        case.assertIn('--simulate', adds[0])
        case.assertEqual(self.actual_adds()[0][self.actual_adds()[0].index('--root') + 1], str(self.target))

    def close(self):
        self.tmp.cleanup()


class HelperContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = module()

    def fixture(self, m=None):
        fixture = Fixture(m or self.m)
        self.addCleanup(fixture.close)
        return fixture

    def test_real_lock_exact_identity_inventory(self):
        lock = self.m.load_lock(LOCK)
        self.assertEqual(len(lock['packages']), 25)
        self.assertEqual(lock['kernel']['version'], '6.12.114-r0')
        self.assertEqual({e['name'] for e in lock['packages'] if e['arch'] == 'noarch'}, self.m.NOARCH)

    def test_clean_orchestration_verify_all_then_strict_offline_add(self):
        f = self.fixture()
        result = f.run()
        f.protocol(self)
        self.assertEqual(result['reference_kernel'], '6.12.114-r0')
        self.assertEqual(len(result['installed_contents']), 3)

    def test_invalid_lock_inventory_arch_trust_and_version_rejected(self):
        for kind in ['missing', 'duplicate', 'extra', 'arch', 'kernel', 'mini', 'key', 'url']:
            with self.subTest(kind=kind):
                f = self.fixture()
                if kind == 'missing': f.lock['packages'].pop()
                if kind == 'duplicate': f.lock['packages'][-1] = copy.deepcopy(f.lock['packages'][0])
                if kind == 'extra': f.lock['packages'].append(copy.deepcopy(f.lock['packages'][0]))
                if kind == 'arch': f.lock['packages'][0]['arch'] = 'aarch64'
                if kind == 'kernel': f.lock['kernel']['version'] = '6.12.113-r0'
                if kind == 'mini': f.lock['native_mini']['sha256'] = '0' * 64
                if kind == 'key': f.lock['native_mini']['keys'].pop()
                if kind == 'url': f.lock['packages'][0]['url'] = 'https://unreviewed.invalid/file.apk'
                with self.assertRaises(ValueError): f.run()
                self.assertFalse(f.calls)

    def test_bad_checksum_and_offline_missing_prevent_any_add(self):
        for mode in ['checksum', 'missing']:
            f = self.fixture()
            path = f.cache / f.lock['packages'][0]['url'].rsplit('/', 1)[-1]
            if mode == 'checksum': path.write_bytes(b'corrupt')
            else: path.unlink()
            with self.assertRaises(ValueError): f.run()
            self.assertFalse(f.actual_adds())

    def test_post_download_independent_checksum_guard(self):
        f = self.fixture()
        f.lock['packages'][0]['sha256'] = '0' * 64
        with self.assertRaises(ValueError): f.run(bypass_download=True)
        self.assertFalse(f.actual_adds())

    def test_signature_failure_prevents_metadata_dryrun_install(self):
        f = self.fixture()
        f.fail = 'verify'
        with self.assertRaises(subprocess.CalledProcessError): f.run()
        self.assertFalse(any('add' in c for c in f.calls))

    def test_signed_metadata_mismatch_and_duplicate_rejected_before_add(self):
        for change in [('pkgver', '1.0-unreviewed'), ('arch', 'aarch64'), ('depend', 'unresolved=1'), ('provides', 'unexpected=1'), None]:
            f = self.fixture()
            entry = f.lock['packages'][0]
            f.package(entry, change=change, duplicate=change is None)
            with self.assertRaises(ValueError): f.run()
            self.assertFalse(any('add' in c for c in f.calls))

    def test_dependency_solver_failure_prevents_actual_install(self):
        f = self.fixture()
        f.fail = 'dry'
        with self.assertRaises(subprocess.CalledProcessError): f.run()
        self.assertFalse(f.actual_adds())

    def test_dryrun_unexpected_state_prevents_actual_add(self):
        f = self.fixture()
        f.bad_dry = True
        with self.assertRaises(ValueError): f.run()
        self.assertFalse(f.actual_adds())

    def bootstrap_witness(self, m, fault):
        with tempfile.TemporaryDirectory(prefix='bootstrap-native-contract-') as tmp:
            p = Path(tmp)
            cache = p / 'cache'
            work = p / 'work'
            cache.mkdir()
            work.mkdir()
            lock = json.loads(LOCK.read_text())
            mini = lock['native_mini']
            archive = cache / mini['url'].rsplit('/', 1)[-1]
            archive.write_bytes(b'synthetic mini fixture')
            values = {}
            for entry in mini['binaries']:
                data = ('tool:' + entry['path']).encode()
                values[entry['path']] = data
                entry.update(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
            for entry in mini['keys']:
                data = ('key:' + entry['file']).encode()
                values['etc/apk/keys/' + entry['file']] = data
                entry.update(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
            def unpack(_archive, native):
                for name, data in values.items():
                    path = native / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(data)
                if fault == 'tool': (native / 'sbin/apk').write_bytes(b'corrupt tool')
                if fault == 'key': (native / 'etc/apk/keys' / mini['keys'][0]['file']).write_bytes(b'corrupt key')
                if fault == 'extra-key': (native / 'etc/apk/keys/extra.rsa.pub').write_bytes(b'extra')
            def checksum(path):
                return mini['sha256'] if path == archive else hashlib.sha256(path.read_bytes()).hexdigest()
            runner = lambda cmd: subprocess.CompletedProcess(cmd, 0, stdout='apk-tools 2.14.12, compiled for x86_64.\n', stderr='')
            with patch.object(m.BUILD, 'download', return_value=None), patch.object(m.BUILD, 'unpack', side_effect=unpack), patch.object(m, 'digest', side_effect=checksum):
                return m.bootstrap(lock, cache, work, True, runner)

    def test_private_bootstrap_command_is_offline_with_empty_repositories(self):
        command = self.bootstrap_witness(self.m, None)
        self.assertIn('--no-network', command)
        self.assertIn('--repositories-file', command)
        self.assertEqual(command[command.index('--repositories-file') + 1], '/dev/null')
        self.assertIn('--keys-dir', command)
        self.assertIn('/native/etc/apk/keys', command[command.index('--keys-dir') + 1])

    def test_private_verifier_tool_key_hash_and_extra_key_fail_closed(self):
        for fault in ['tool', 'key', 'extra-key']:
            with self.subTest(fault=fault):
                with self.assertRaises(ValueError): self.bootstrap_witness(self.m, fault)

    def test_nonempty_and_symlink_targets_rejected(self):
        f = self.fixture()
        (f.target / 'existing').write_bytes(b'preserve')
        with self.assertRaises(ValueError): f.run()
        self.assertFalse(f.calls)
        f2 = self.fixture()
        f2.target.rmdir()
        f2.target.symlink_to(f.target, target_is_directory=True)
        with self.assertRaises(ValueError): f2.run()
        self.assertFalse(f2.calls)

    def test_native_add_failure_is_not_rescued(self):
        f = self.fixture()
        f.fail = 'add'
        with self.assertRaises(subprocess.CalledProcessError) as error: f.run()
        self.assertEqual(error.exception.returncode, 9)
        self.assertEqual(len(f.actual_adds()), 1)

    def test_installed_hash_abi_and_inventory_checked(self):
        for mode in ['contents', 'inventory']:
            f = self.fixture()
            f.bad_contents = mode == 'contents'
            f.bad_inventory = mode == 'inventory'
            with self.assertRaises(ValueError): f.run()
            self.assertEqual(len(f.actual_adds()), 1)
        f = self.fixture()
        f.lock['kernel']['contents'][1]['vermagic'] = '6.12.113-0-virt wrong'
        with self.assertRaises(ValueError): f.run()

    def test_semantic_mutants_not_setup_failures(self):
        source = HELPER.read_text()
        specs = [
            ('bootstrap-network', "'--no-network']\n", "'--no-progress']\n", 'bootstrap-protocol'),
            ('bootstrap-repositories', "'--repositories-file', '/dev/null',", "'--repositories-file', '/unexpected/repositories',", 'bootstrap-protocol'),
            ('private-tool', "        check_file(native / entry['path'], entry)\n", '        pass\n', 'bootstrap-tool'),
            ('trust-key-hash', "        check_file(keys / entry['file'], entry)\n", '        pass\n', 'bootstrap-key'),
            ('trust-key-set', "    require({p.name for p in keys.iterdir()} == {e['file'] for e in mini['keys']}, 'Verifier trust-key set mismatch')\n", '    pass\n', 'bootstrap-extra-key'),
            ('checksum', '            check_file(path, entry)\n', '            pass\n', 'checksum'),
            ('signature', "        runner(apk + ['verify'] + files)\n", '        pass\n', 'signature'),
            ('identity', '            inspect_metadata(Path(path), entry)\n', '            pass\n', 'identity'),
            ('closure', '        runner(apk + flags + files)\n', '        pass\n', 'closure'),
            ('scripts', "                      '--no-scripts', '--no-chown', 'linux-virt=' + VERSION] + files)", "                      '--no-chown', 'linux-virt=' + VERSION] + files)", 'flags'),
            ('network', "        runner(apk + ['--root', str(target)", "        runner([v for v in apk if v != '--no-network'] + ['--root', str(target)", 'flags'),
            ('contents', '        contents = verify_contents(target, lock)\n', '        contents = []\n', 'contents'),
            ('inventory', '        installed_inventory(target, lock)\n', '        pass\n', 'inventory'),
        ]
        for name, before, after, witness in specs:
            if source.count(before) != 1:
                raise RuntimeError('mutant selection setup mismatch: ' + name)
            mutant = module(source.replace(before, after, 1))
            f = self.fixture(mutant)
            # Fixture/module creation and all setup are outside the assertion block.
            if witness == 'checksum': f.lock['packages'][0]['sha256'] = '0' * 64
            if witness == 'signature': f.fail = 'verify'
            if witness == 'identity': f.package(f.lock['packages'][0], ('pkgver', 'wrong'))
            if witness == 'closure': f.fail = 'dry'
            if witness == 'contents': f.bad_contents = True
            if witness == 'inventory': f.bad_inventory = True
            with self.subTest(mutant=name):
                try:
                    if witness == 'bootstrap-protocol':
                        command = self.bootstrap_witness(mutant, None)
                        self.assertIn('--no-network', command)
                        self.assertEqual(command[command.index('--repositories-file') + 1], '/dev/null')
                    elif witness.startswith('bootstrap-'):
                        with self.assertRaises(ValueError): self.bootstrap_witness(mutant, witness[len('bootstrap-'):])
                    elif witness == 'flags':
                        f.run()
                        f.protocol(self)
                    else:
                        error = subprocess.CalledProcessError if witness in ['signature', 'closure'] else ValueError
                        with self.assertRaises(error): f.run(bypass_download=witness == 'checksum')
                except AssertionError:
                    print('KILLED semantic mutant:', name, flush=True)
                else:
                    self.fail('semantic mutant survived: ' + name)


class OracleWiring(unittest.TestCase):
    def test_helper_and_new_module_path_only_consumer_unchanged(self):
        source = (ROOT / 'scripts/evdev-linux-oracle.sh').read_text()
        self.assertNotIn('--repository https:', source)
        self.assertIn('python3 scripts/evdev-linux-oracle-packages.py', source)
        self.assertIn('lib/modules/6.12.114-0-virt/', source)
        self.assertNotIn('lib/modules/6.12.113-0-virt/', source)
        original_tail = LEGACY[LEGACY.index('cp "$S/bin/busybox"'):].replace('6.12.113-0-virt', '6.12.114-0-virt')
        self.assertTrue(source.endswith(original_tail), 'Consumer/S-root copies/init/QEMU90/strict RC acceptance changed')

    def test_workflow_only_one_new_host_invocation_each(self):
        line = '          PYTHONDONTWRITEBYTECODE=1 python3 scripts/evdev-linux-oracle-packages-host-tests.py\n'
        for name in ['alpine.yml', 'release.yml']:
            source = (ROOT / '.github/workflows' / name).read_text()
            self.assertEqual(source.count(line), 1)
            self.assertIn('python3 scripts/alpine-strace-tail-host-tests.py\n' + line, source)


if __name__ == '__main__':
    unittest.main(verbosity=2)
