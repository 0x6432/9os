"""Linux-native test-process ownership; not a guest/runtime helper.

A new session owns each disposable control tree. Temporarily becoming a child
subreaper lets this single-threaded harness reap grandchildren after killing a
stalled supervisor, rather than relying on container PID 1. Only that process
group is signaled/waited; unrelated children are never swept. Controls must not
escape their group (this is cleanup, not a malicious-process sandbox).
"""
from contextlib import contextmanager
import ctypes
import os
import signal
import subprocess


@contextmanager
def _subreaper():
    libc = ctypes.CDLL(None, use_errno=True)
    prctl = libc.prctl
    prctl.argtypes = [ctypes.c_int] + [ctypes.c_ulong] * 4
    prctl.restype = ctypes.c_int
    previous = ctypes.c_int()

    def call(option, value):
        if prctl(option, value, 0, 0, 0) != 0:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error))

    call(37, ctypes.cast(ctypes.pointer(previous), ctypes.c_void_p).value)  # PR_GET_CHILD_SUBREAPER
    call(36, 1)  # PR_SET_CHILD_SUBREAPER
    try:
        yield
    finally:
        call(36, previous.value)


def _group_exists(pgid):
    try:
        os.killpg(pgid, 0)
    except ProcessLookupError:
        return False
    return True


def _reap_group(pgid, *, block):
    reaped = []
    while True:
        try:
            pid, status = os.waitpid(-pgid, 0 if block else os.WNOHANG)
        except InterruptedError:
            continue
        except ChildProcessError:
            return reaped
        if pid == 0:
            return reaped
        reaped.append((pid, status))


def _cleanup(proc):
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    # All controls inherit this isolated group, including their stdout/stderr.
    # Killing the whole group closes those pipes before communicate/reaping.
    out, err = proc.communicate()
    reaped = _reap_group(proc.pid, block=True)
    if _group_exists(proc.pid):
        raise RuntimeError('isolated native control group survived SIGKILL/reap')
    return out, err, reaped


def run_isolated(command, *, timeout):
    """Return a CompletedProcess, or re-raise the original failure after cleanup.

    Timeout cleanup is never a retry/rescue/acceptance result. In particular a
    timeout=6 still fails at six seconds, not at the consumer's five-second
    watchdog. The failure carries cleanup evidence for dedicated controls.
    """
    with _subreaper():
        proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, start_new_session=True)
        try:
            out, err = proc.communicate(timeout=timeout)
            adopted = _reap_group(proc.pid, block=False)
            if adopted or _group_exists(proc.pid):
                raise RuntimeError('native supervisor exited without reaping its control descendants')
        except BaseException as failure:
            out, err, adopted = _cleanup(proc)
            failure.cleanup_pgid = proc.pid
            failure.cleanup_reaped = adopted
            failure.cleanup_returncode = proc.returncode
            failure.cleanup_stdout = out
            failure.cleanup_stderr = err
            raise
        return subprocess.CompletedProcess(command, proc.returncode, out, err)
