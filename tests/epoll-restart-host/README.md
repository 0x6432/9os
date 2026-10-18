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
