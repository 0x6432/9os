# Caught-signal epoll restart regression

`userland/tests/epollrestarttest.c` is a standalone Linux-ABI consumer. Compile as
an ordinary tiny userland C test; no kernel/VM build or dependency restore is
needed for the native reference:

```
cc -std=gnu11 -O2 -Wall -Wextra -Werror userland/tests/epollrestarttest.c -o /tmp/epollrestarttest
timeout 7 /tmp/epollrestarttest
```

It runs ten cases: epoll_wait with caught SA_RESTART off/on; epoll_pwait and
raw epoll_pwait2 with caught SA_RESTART off/on, both without and with a temporary
mask. Every wait is finite100ms; a one-shot CLOCK_MONOTONIC timer sends SIGUSR1
at20ms. SIGUSR2 is a persistent mask sentinel. Each case checks EINTR, exactly
one handler, handler mask, full post-sigreturn original-mask membership, and an
unblocked watchdog. There is no elapsed-time acceptance heuristic or infinite
wait. A separate SIGALRM default-action process watchdog is fixed at5seconds;
per-case timers never replace it. The external7second native wrapper is only an
additional supervisor, not a changed test deadline.

Raw kernel sigsets are8bytes on supported64-bit targets. Targets without the
legacy epoll_wait syscall use the public API's epoll_pwait-with-null-mask ABI;
no syscall number is invented. Headers must provide epoll_pwait2. Missing syscall
support or failed timer/signal setup is a failure, not a skipped pass.

`evidence/native-linux-before-fix.log` records the native Linux reference:
72 checks,0 failures, exit0. It was committed with the consumer BEFORE editing
`kernel/core/signal.c`. Native Linux proves the expected contract, NOT 9os guest
behavior. Parent must preserve a real original-policy guest outcome using that
first commit before applying the later policy fix. No guest was run locally.

This targets an inherited caught-SA_RESTART ABI issue, not cold startup. All
startup/integration holds remain unchanged. No shared workflow wiring is added.

## Policy-only checkpoint and exact-function controls

The test-before-fix commit is
`8bef592aa898fa3b2164ca179e0b5633f6b5bc17`. It includes the consumer and first
native reference, with **no signal policy edit**. Parent must use that commit
for the original-policy guest baseline, and preserve its outcome before applying
the subsequent fix. Local host negative classification is NOT that baseline.

The later kernel edit adds only epoll_pwait/epoll_pwait2 and the conditionally
available epoll_wait to `no_restart()`. It changes no scheduler, enrollment,
signal-frame, mask restoration, or anonfd code. This deliberately fixes an
inherited Linux caught-SA_RESTART policy issue, not a startup issue.

```
python3 tests/epoll-restart-host/run.py --mutations
```

The runner extracts the exact production `no_restart()` body, compiling only
tiny native x86 controls. It checks every syscall number through NR_SYSCALLS,
including preservation of previous exclusions and ordinary restartable calls.
The second native variant undefines only the legacy epoll_wait macro to check
the ifdef: it is not an aarch64/riscv64 build. Required macro presence in those
headers is inspected as text only.

Recorded results in `evidence/`:

- `original-policy-host-negative.log`: exact original policy fails3 explicit
  epoll exclusions, exit1 as expected (classification only).
- `fixed-policy-host.log`:461 checks/0 failures with the legacy syscall present,
  and461/0 with it absent; six named mutations rejected in11 executions by explicit check
  mismatches across meaningful variant combinations, not compiler crashes.
  Mutations remove all three new exclusions, omit each one separately, drop
  ppoll's existing exclusion, and incorrectly exclude ordinary read.
- `native-linux-after-policy-edit.log`: unchanged consumer passes72/72 on native
  Linux again. This does NOT execute or validate the changed 9os kernel.

Real baseline/fixed 9os signal delivery, handler return, full mask restoration,
and target integration remain unexecuted here; parent owns those runs. No shared
workflow/Makefile, VM, heavy build, non-x86 build/run or dependency install was
used. Tests are unsanitized and make no sanitizer-pass claim. Parent owns pushes,
PRs, integration, and all existing holds.
