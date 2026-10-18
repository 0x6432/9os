# Independent review: standalone epoll ABI guest consumer

## Scope / verdict

Read-only review of `/data/9os-team-epoll` commit
`d7ef6647d913cb34ec12900cf0d64fa39d7654d4`, especially
`userland/tests/epollabitest.c` and
`tests/epoll-abi-host/guest-watchdog-control.py`.
Consumer SHA256:
`73f2714d5c39b744a2c6fee1432b5c01e9992462fd66fc5c84a17203805953cc`.
The original native/shared-case sources and earlier model documentation were
also inspected; this is not an independent audit of every preceding kernel change.

**No blocking defect found in the consumer's ABI/full-mask assertions or normal
watchdog control. One medium-severity native-harness cleanup issue is actionable
below.** All successful execution in this review is native x86 Linux reference
or intentionally mutated native test code, NOT guest/kernel/cross-architecture
validation. No model run was performed for this review.

## R1 — medium: outer six-second native cleanup limit does not clean descendants

Location: `tests/epoll-abi-host/guest-watchdog-control.py:39–40`, and the README's
six-second cleanup-limit description.

`subprocess.run(..., capture_output=True, timeout=6)` kills/reaps its direct child
on timeout but does not terminate the consumer's child. There is no isolated
process group or explicit descendant cleanup in this script. The current
negative control passes when the production supervisor is healthy; the gap is
on the harness's exceptional cleanup path if that supervisor stalls/regresses.
It does not invalidate the observed five-second native watchdog success.

Independent reproducible control: retained the script's signal-blocked child
hang, then additionally stalled the parent supervisor in a temporary source
copy. Used the same capture_output/timeout6 subprocess pattern. Observed:

```text
six-second subprocess.run cleanup timed out at 6.006814s
After subprocess.run timeout killed/reaped only the supervisor,
its signal-blocked child was still running (waitpid WNOHANG=0).
```

The independent wrapper used a new process session and Linux child-subreaper
mode; its finally block killed the isolated process group and reaped the adopted
child. No control process was left running. This additional supervisor stall is
an intentional test mutation, not a spontaneous production/guest failure.
Source and output are preserved in
`/data/team-coordination/epoll-consumer-review-controls/`:
`watchdog-supervisor-stall-control.c`, `watchdog-cleanup-control.log`.

Action: keep the five-second consumer deadline unchanged. Run the native control
in its own session/process group using Popen, catch TimeoutExpired, kill the whole
isolated process group, and wait/reap before raising the failure. Add a supervisor-
stall cleanup control so the outer timeout's cleanup guarantee is tested. Do not
turn a timeout into a successful watchdog-control result or extend acceptance.
This is a harness robustness fix, not a requested kernel/guest behavior change.

## ABI and full-mask review — pass within stated coverage

- Public syscall-number fallbacks match repository Linux ABI definitions:
  epoll_pwait281 on x86_64,22 on aarch64/riscv64; epoll_pwait2=441.
- The ILP32/unsupported-architecture compile gates, explicit two-int64 timespec
  and architecture-specific epoll_event size/offset assertions are appropriate.
  Raw mask input is exactly8 bytes; libc sigset_t padding is not compared.
- Error/precedence and huge-timeout cases are carried over to the standalone
  consumer. Nonzero/setup/child-signal outcomes cannot print a successful exit.
- `same_mask` at lines125–133 compares every signal member1..64, not only
  SIGALRM. Native independent raw-bit tests detected all64 single-bit differences
  and accepted equal masks. This helper test uses the x86 Linux bit layout only;
  it is not cross-architecture libc validation.
- Runtime test setup seeds blocked SIGUSR1. Interrupted calls derive expected
  old mask plus SIGALRM, use a zero temporary raw mask, verify EINTR, handler
  execution, and complete restoration. A later explicit restoration does not
  erase the already counted check failure. Setup/mask-read failures exit2.
- Independent snapshot mutations adding SIGUSR2 or removing seeded SIGUSR1 each
  caused six explicit restoration mismatches and exit1. These inject corruption
  into the observed userspace snapshot; they demonstrate assertion sensitivity,
  not an actual kernel mask-restoration regression.
- The finite2,000,001ns interval is measured before check/console output. Its
  result and restoration are checked separately. Serial printing cannot pad
  that measured interval. Scheduling/clock resolution still limit any timing
  test's ability to prove absence of every early-return bug.
- Coverage limitation: ready calls assert result counts, not returned event
  payload/flags; the mask/time boundary consumer is not complete epoll semantics
  or a proof of the filtered wake path. Very long waits interrupted after30ms
  cannot alone prove the exact internal saturation value; separate model coverage
  is distinct from these public-ABI observations.

## Watchdog review — normal control passes; external supervision still required

- Parent and child have separate ITIMER_REAL/masks after fork. Only the child
  arms the30ms SIGALRM timer; the parent uses monotonic elapsed time.
- The parent retains one start time, five-second threshold, WNOHANG polling and
  short nanosleeps; it does not reset its deadline on retries or use a competing
  SIGALRM. Child exit status is propagated; unexpected signals fail.
- Supplied negative control changes only the child call site to a signal-blocked
  permanent pause and keeps the production parent text. Independently observed:

```text
epollabitest: FAIL five-second process watchdog
exit=124; observed elapsed=5.001723s
PASS: original five-second supervisor rejected signal-blocked child hang
```

- This is evidence for an interruptible, killable native child. The threshold is
  not a universal hard bound on guest wall-clock completion: stderr I/O and
  `stop_child`'s blocking waitpid at lines202–204 still depend on a functioning
  kernel/scheduler/kill/reap path. Keep an independent guest/VM harness timeout
  for a wedged guest. The child also prints inside the overall five-second process
  budget, so actual slow-serial/SMP/MLFQ guest behavior remains unmeasured; do not
  silently lengthen or waive this deadline if it fails there.

## Executed native controls and preserved first fixture failure

Tiny native compilation used only cc and libc, no kernel/library/image builds.
Unmodified consumer flags:
`-std=gnu11 -O2 -Wall -Wextra -Werror -D_FORTIFY_SOURCE=2`.

```text
Unmodified native Linux consumer: exit=0; elapsed=0.184119s
  epollabitest: 78 checks, 0 failures
Original guest-watchdog-control.py: harness exit=0
  intentional hung child exit=124 at5.001723s
Full-mask raw-bit helper: all64 single-bit differences detected
Extra-SIGUSR2 snapshot mutation: exit1, six restoration mismatches
Lost-SIGUSR1 snapshot mutation: exit1, six restoration mismatches
Supervisor-stall cleanup control: exposed surviving child at6.006814s;
  independent group cleanup/reaping completed successfully
```

The initial independent all-bits fixture exited2 because it attempted libc
sigaddset for reserved glibc signals32/33. The unmodified consumer had already
passed78checks before that fixture error. Diagnostic confirmation:
`sigaddset(32)` and `sigaddset(33)` both return-1/EINVAL;31/34 succeed.
Original `full-mask-first.c` and `full-mask-first-failure.txt` are retained in the
controls directory. The corrected fixture sets each raw public bit in a copied
sigset snapshot and retains the exact production comparator; no assertion was
weakened, no production source fixed, and the original failed fixture is not
counted as a pass. Corrected source plus both snapshot-mutation sources/logs
are retained alongside it.

## Remaining guest/CI work and ownership

No VM, guest execution, heavy build, dependency restoration, sanitizer run,
non-x86 compile/test, credential access, CI action, push or worktree modification
occurred. Only disposable/native test outputs and this coordination report were
written. Original consumer/control/evidence files were not changed.

Parent must validate the standalone consumer against actual guest libc/syscall
behavior and retain x86_64/aarch64/riscv64, source/runtime, MLFQ/SMP and original
regression requirements. Parent owns integration/CI wiring and the x86 heavy
slot. Previous model results are not relabeled as guest evidence; previous
failures and M40/cold-start holds remain unchanged.
