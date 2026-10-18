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
