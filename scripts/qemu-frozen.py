"""Narrow x86 QEMU argv and byte-identity checks; no build or guest execution here.

The manifest is a review input, not an authentication signature or a substitute
for inspecting disk metadata, ISO configuration, host isolation, or the launcher.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import stat
import subprocess

SHA = re.compile(r'[0-9a-f]{64}\Z')


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def regular(path, label):
    if not isinstance(path, str) or not os.path.isabs(path) or any(c in path for c in ',\r\n\x00'):
        raise ValueError(label + ' requires a safe absolute path')
    p = Path(path)
    if not stat.S_ISREG(p.lstat().st_mode):
        raise ValueError(label + ' must be a regular non-symlink file')
    return p


def socket_path(value, label):
    if not isinstance(value, str) or not os.path.isabs(value) or any(c in value for c in ',\r\n\x00'):
        raise ValueError(label + ' requires a safe absolute path')
    p = Path(value)
    if p.exists() or p.is_symlink() or not p.parent.is_dir():
        raise ValueError(label + ' must be a fresh socket in an existing directory')
    return value


def load(manifest, disk, monitor, qmp):
    mpath = regular(manifest, 'manifest')
    with mpath.open('rb') as source:
        raw = source.read(16385)
    if len(raw) > 16384:
        raise ValueError('manifest exceeds size bound')
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result: raise ValueError('duplicate manifest key: ' + key)
            result[key] = value
        return result
    m = json.loads(raw, object_pairs_hook=unique)
    if not isinstance(m, dict) or set(m) != {'version', 'qemu', 'iso', 'golden_disk'} or type(m['version']) is not int or m['version'] != 1:
        raise ValueError('invalid frozen manifest shape')
    paths = {}
    for key in ('qemu', 'iso', 'golden_disk'):
        item = m[key]
        if not isinstance(item, dict) or set(item) != {'path', 'sha256'} or not isinstance(item['sha256'], str) or not SHA.fullmatch(item['sha256']):
            raise ValueError('invalid ' + key + ' manifest entry')
        paths[key] = regular(item['path'], key)
        if digest(paths[key]) != item['sha256']:
            raise ValueError(key + ' hash mismatch')
    if not os.access(paths['qemu'], os.X_OK):
        raise ValueError('qemu executable is not executable')
    work = regular(disk, 'disposable disk')
    if any(os.path.samefile(work, p) for p in paths.values()):
        raise ValueError('disposable disk aliases a frozen fixture')
    if digest(work) != m['golden_disk']['sha256']:
        raise ValueError('disposable disk does not match the golden disk')
    if len({(p.stat().st_dev, p.stat().st_ino) for p in paths.values()}) != len(paths):
        raise ValueError('frozen fixture files must be distinct')
    monitor = socket_path(monitor, 'monitor')
    qmp = socket_path(qmp, 'qmp')
    if monitor == qmp:
        raise ValueError('monitor and qmp sockets must differ')
    # Preserve Makefile x86 default device order and KMS runner's 4 CPU/3072M.
    cmd = [str(paths['qemu']), '-M', 'q35', '-cpu', 'qemu64,+smep,+smap,+rdrand',
           '-m', '3072M', '-smp', '4', '-serial', 'stdio', '-no-reboot',
           '-device', 'virtio-keyboard-pci', '-device', 'virtio-tablet-pci',
           '-netdev', 'user,id=n0', '-device', 'virtio-net-pci,netdev=n0',
           '-boot', 'd', '-cdrom', str(paths['iso']),
           '-audiodev', 'none,id=snd0', '-device', 'virtio-sound-pci,audiodev=snd0,streams=1',
           '-display', 'none', '-monitor', 'unix:' + monitor + ',server,nowait',
           '-qmp', 'unix:' + qmp + ',server,nowait',
           '-drive', 'file=' + str(work) + ',if=none,id=vd0,format=raw',
           '-device', 'virtio-blk-pci,drive=vd0,serial=9os-disk0']
    return cmd, (mpath, hashlib.sha256(raw).hexdigest(), m, paths)


def verify_after(state):
    mpath, manifest_hash, m, paths = state
    if digest(regular(str(mpath), 'manifest')) != manifest_hash:
        raise ValueError('manifest changed during run')
    for key, path in paths.items():
        if digest(regular(str(path), key)) != m[key]['sha256']:
            raise ValueError(key + ' changed during run')


def run_host(command, timeout):
    """Host helpers get a separate process group and bounded failure cleanup.

    This does not contain descendants that deliberately create new sessions.
    """
    p = subprocess.Popen(command, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         start_new_session=True)
    cleanup_errors = []
    signalled = False
    output, passed = b'', False
    try:
        try:
            output, _ = p.communicate(timeout=timeout)
            passed = p.returncode == 0
        except subprocess.TimeoutExpired:
            try:
                os.killpg(p.pid, signal.SIGKILL)
                signalled = True
            except ProcessLookupError:
                signalled = True
            except OSError as e:
                cleanup_errors.append('initial group kill: ' + str(e))
            try:
                output, _ = p.communicate(timeout=10)
                output += b'\n(timeout)\n'
            except subprocess.TimeoutExpired as e:
                # A descendant can keep the pipe open after group kill. This
                # exception carries cumulative bytes, not just the last read.
                output = (e.output or b'') + b'\n(timeout; output pipe still open, incomplete)\n'
                try: p.stdout.close()
                except OSError as e: cleanup_errors.append('output pipe close: ' + str(e))
    finally:
        # Do not mask an earlier exception or silently claim cleanup succeeded.
        if not signalled:
            try: os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            except OSError as e: cleanup_errors.append('final group kill: ' + str(e))
        try:
            if p.poll() is None: p.wait(timeout=10)
        except (OSError, subprocess.TimeoutExpired) as e:
            cleanup_errors.append('helper reap: ' + str(e))
        if cleanup_errors:
            # If an exception is already propagating, preserve it while making
            # cleanup errors visible; otherwise attach them to the failed step.
            import sys
            if sys.exc_info()[0] is not None:
                print('host helper cleanup failed: ' + '; '.join(cleanup_errors), file=sys.stderr)
    if cleanup_errors:
        output += ('\n(host helper cleanup failed: ' + '; '.join(cleanup_errors) + ')\n').encode('utf-8', errors='replace')
        passed = False
    return output, passed


def root_gate_command(mounts='/proc/mounts', dbus='/usr/libexec/dbus-daemon-launch-helper',
                      pam='/usr/sbin/unix_chkpwd', shadow='/etc/shadow'):
    """Require disk root, repaired helpers, and successful shadow-group setup."""
    q = shlex.quote
    program = '$2 == "/" { n++; if ($1 == "/dev/vda1" && $3 == "ext2") ok++ } END { exit !(n == 1 && ok == 1) }'
    return ('awk ' + q(program) + ' ' + q(mounts)
            + ' && test "$(stat -c %u:%g:%a ' + q(dbus) + ')" = 0:81:4750'
            + ' && test "$(stat -c %u:%g:%a ' + q(pam) + ')" = 0:42:2755'
            + ' && test ! -L ' + q(shadow) + ' && test -f ' + q(shadow)
            + ' && test "$(stat -c %u:%g:%a ' + q(shadow) + ')" = 0:42:640')
