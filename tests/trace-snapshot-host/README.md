# Failure-only single-thread register snapshot

`trace-snapshot TID` uses public Linux `PTRACE_SEIZE`, `PTRACE_INTERRUPT`,
`GETREGSET/NT_PRSTATUS` and `PEEKDATA`. It never writes memory/registers, steps,
requests EXITKILL, requests focus or claims input consumption. The register ABI
is explicit for x86_64, aarch64 and riscv64. Local builds/tests are x86_64 only;
PR CI owns other architectures.

The wait is bounded at two seconds/400 polls. Output is bounded to one register
set, 256 mapping chunks of at most 1023 bytes and 32 stack words. Truncation,
unreadable memory, short register sets and errors are explicit failures. Raw
stack words are **not unwound frames**; mapping offsets require the matching
signed package ELF, not a guessed symbol. A single sample cannot prove a deadlock
or explain why startup stalled. Only one thread is stopped; this is not a
consistent process-wide snapshot.

A normal interrupt stop is detached with signal zero. Unexpected signal/group
stops are rejected and detached with their signal. If no stop arrives, output
explicitly says release-on-tracer-exit, not clean explicit detach. External TERM
or KILL also relies on normal tracer-exit release; no EXITKILL option is enabled.
Attaching changes scheduling. The existing readiness result is irrevocable
before three failure-only samples (one second apart), each externally bounded
by `timeout -k 1 -s TERM 4`. No samples run on successful startup, and the failed
probe still exits 1 regardless of diagnostic results. No startup deadline,
physical sender condition or acceptance gate changes.

Native controls compile only tiny consumers/fixtures, not any desktop library:

```
python3 tests/trace-snapshot-host/test-native.py --consumer native
python3 tests/trace-snapshot-host/test-native.py --consumer signed-musl
python3 scripts/trace-snapshot-integration-host-tests.py
python3 tests/eventfd-host/install-model.py
```

Both consumer configurations passed 13 native-kernel controls: nine invalid CLI
forms, nonexistent target, busy synthetic target, blocked synthetic target and
pre-existing SIGSTOP. They assert complete registers/raw stack words, executable
PC/readable SP mappings, explicit detach, TracerPid zero, original liveness and
preserved pre-existing stop. Yama PR_SET_PTRACER is requested by the explicit
synthetic fixture where supported; an EINVAL response (no such facility) never
skips the actual ptrace permission or snapshot assertions. These are **not 9os,
Konsole, KDE, physical-input or reliability evidence**. Shell controls additionally
retain failed exit for diagnostic statuses 0/1/124/143, all three samples, exact
bounds and placement after the frozen failure witness. Installer controls reject
missing/duplicate builds and require strict flags/nested source isolation.

Motivation: the one fixed integrated selector trial at bcdf360 failed 8/18 with
valid frozen hashes, pty=0/terminal=0, genuine Konsole PID901 alive with six
threads, no PTY or AT-SPI registration. Bulk readiness did not run because its
PTY prerequisite was absent. Failure-only strace showed some syscall progress,
not a cause. Evidence-only commit aeeaeb963896d978f3f770f962f1af782253f639 retains
the full failure; earlier private positives cannot replace it. This diagnostic
is not a fix or retry-until-pass strategy. All M40 reliability/integration holds
remain open.

The desktop guest CI also runs `trace-snapshot-selftest` as root and UID1000
on all three architectures. Each synthetic target's direct parent is replaced
by the helper, so this requires no global Yama changes. Busy/blocked targets
must resume with TracerPid zero and 32 stack words; SIGSTOP must be preserved.
Those guest jobs are pending until current-head CI reports them; native controls
are not substituted for guest results.
