# Linux epoll public-ABI boundary tests (x86 host only)

This directory is deliberately standalone: no kernel, VM, cross build, service,
startup fixture, or shared CI workflow is invoked.

## Run

```sh
python3 tests/epoll-abi-host/run.py --mode native
python3 tests/epoll-abi-host/run.py --mode model --mutations
```

Uses the preinstalled `CC` (default `cc`) for tiny C programs only. Native mode
requires Linux x86_64 with raw `epoll_pwait2` support. Each execution has a fixed
5-second subprocess watchdog; interruption tests use a 30-ms process-local alarm.
No original project validation/deadline is changed. Optional `--sanitize` needs
an installed UBSan runtime; it is **not available on the development host**.

- `native.c` invokes raw Linux syscalls (not libc wrappers that hide the kernel's
  8-byte sigset ABI) and normalizes errors to negative errno. `cases.h` supplies
  common expected return values for native and model runs. No reference result
  is inferred from a 9os result.
- `run.py --mode model` extracts **the unchanged text** from `ep_set_mask` to the
  memfd section of the current production `kernel/fs/anonfd.c`; it does not
  reimplement the timeout conversion or syscall bodies. The extracted SHA256 is
  in the log. `--source` can select the preserved baseline section.
- `model-pre.c` supplies mock clock, copies, allocation, scan, locks, FD lifetime,
  poll epoch/enrollment and thread state. `model-post.c` checks exact durations,
  finite saturation versus infinite sentinel, retry deadline accounting, mask
  strip/restore/deferred EINTR restore, error cleanup, filter and epoch forwarding.
  Seven independent temporary mutated syscall variants must fail explicit checks.
  The real production source and fixtures are never mutated by this operation.

## Concrete behavior and fix

Linux rejects a non-null mask with a size other than 8 with EINVAL, before copying
it; a null mask ignores size. `epoll_pwait2` copies/validates the time before mask,
FD and maxevents checks. Negative seconds, negative nanoseconds and nanoseconds
at least one billion fail EINVAL even with an immediately ready event. Arbitrarily
large valid positive seconds are accepted and must not become a zero, negative,
or wrapped wait.

The original implementation ignored mask size and converted unchecked timespecs
through a signed `int` millisecond timeout. The fix shares the existing wait body
through an internal nanosecond helper, validates before arithmetic, and saturates
finite conversion/addition at Linux's signed nanosecond horizon (`INT64_MAX`),
keeping `UINT64_MAX` exclusively for infinite waits. The kernel's monotonic clock
is nanoseconds since boot; like Linux's ktime model, finite deadlines at/above
that horizon are saturated. A saturated deadline is not represented as infinite.
The finite deadline is created once, not extended by scans/spurious/epoch retries.

The scan, ET/ONESHOT state handling, direct-leaf filter construction, fd pinning,
BKL protocol, poll epoch/enrollment implementation, mask swap and deferred EINTR
restoration logic remain unchanged. The pre-existing maxevents limit of 4096
is outside this narrowly scoped fix; this is not a claim of complete epoll ABI
conformance.

## Evidence and limits

See `evidence/README.md` and exact logs. Baseline checkpoint `f5d3b1b` captures the
original tests/source and first failure. Production fix checkpoint `1e43e4e`
also preserves the extended test sources when dynamic-UBSan startup failed.

**Native Linux is reference only; the extracted model is not 9os runtime proof.**
Guest signal delivery/return, concurrent scheduler enrollment and mask semantics,
real ET/ONESHOT delivery and direct-leaf wake behavior, kernel integration and
non-x86 behavior have not been executed by this work. No heavy/VM slot was
requested or used. Parent owns CI wiring, guest validation, push and integration;
these tests are not wired into a shared workflow here. PR40 cold remains failed;
there is no cold cure or M40 completion claim.

## Standalone guest consumer (not yet guest-validated)

`userland/tests/epollabitest.c` is a single-file public-ABI consumer derived from
the preserved native source and shared cases at `6f0fcef`. It needs only libc,
with no project include path or generated/binary input:

```sh
cc -std=gnu11 -O2 -Wall -Wextra -Werror -D_FORTIFY_SOURCE=2 \
  userland/tests/epollabitest.c -o /tmp/epollabitest
/tmp/epollabitest
```

The source supports the public Linux x86_64, aarch64 and riscv64 64-bit ABIs:
it uses signed 64-bit raw timespec fields, libc's architecture-correct epoll-event
layout with compile-time checks, an explicit 8-byte kernel sigset, and public
syscall-number fallbacks for older libc headers. It introduces no threading or
additional link dependency. The normal mkroot standalone-source mechanism can
compile it; Alpine's explicit build/runtime lists and all CI wiring remain
parent-owned and **were not edited**.

The same raw errno/precedence/huge-timeout checks are retained. Signal restoration
checks now compare all 64 kernel-visible signal members (not libc padding), with
an extra blocked SIGUSR1 seed to exercise preservation of a nontrivial old mask.
The finite timeout measures the syscall interval **before any console I/O**, so
slow serial output cannot conceal an immediate/early return.

A separate supervisor retains the original **five-second process watchdog**
without sharing the test child's 30-ms ITIMER_REAL signal timer. A deliberately
signal-blocked hung child was killed/reaped with exit 124 by the unchanged
supervisor at 5.001805 seconds in the native negative control. The outer native
harness retains a six-second **failure** limit; the consumer deadline remains
five seconds. `guest-watchdog-control.py` recreates that temporary child-only
mutation without modifying the runtime source or any existing fixture. The
original subprocess.run timeout did not clean grandchildren if the supervisor
stalled; that medium-severity review finding is retained under
`evidence/harness-cleanup/`, not erased by the normal control pass.

The repaired runner uses `isolated_process.py`: Popen with a new session/process
group and temporary Linux child-subreaper ownership. Exceptions kill/reap the
whole control group and re-raise the original failure; there is no retry, rescue
or longer acceptance window. Normal exit also rejects leftover control processes.
Reaping is restricted to that isolated group, not unrelated harness children.
The prior subreaper setting is restored. This Linux-native cleanup helper is not
part of the portable guest consumer or a sandbox for processes escaping a group.

```sh
python3 -B tests/epoll-abi-host/guest-watchdog-control.py
python3 -B tests/epoll-abi-host/test-guest-watchdog-cleanup.py
```

The second dedicated regression control SIGSTOPs a temporary supervisor while
its signal-blocked child hangs. It requires the original six-second
TimeoutExpired to be raised **after** both processes/group are gone and the
adopted child has been reaped, without touching an unrelated live child or
changing prior subreaper state. It catches this expected failure only for test
assertions; normal native-control timeouts still fail. Exact first repaired-run
logs are in `evidence/harness-cleanup/`.

Native-only evidence: `guest-consumer-linux-reference.log` records **78 checks,
0 failures**; `guest-watchdog-linux-control.log` records the expected watchdog
rejection. Both include exact source hashes. These are **not guest results**.
No kernel build, musl/Alpine build, VM, non-x86 compilation/execution or sanitizer
run was performed for this addition. Parent must run signed build, source/runtime,
MLFQ/SMP and cross-architecture CI; no guest pass is claimed here.
