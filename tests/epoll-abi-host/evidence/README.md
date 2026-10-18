# First-observation evidence

Baseline production source: `18ef21fc34945f482d262993aa33690070aa86d5`.
`baseline-syscalls.c.txt` preserves the exact syscall section before fixes.

- `linux-reference.log`: first run of `cc -O2 -Wall -Wextra -Werror tests/epoll-abi-host/native.c -o /data/9os-epoll-abi-native && timeout 5 /data/9os-epoll-abi-native`, x86_64 Linux 6.18.49. 66 checks, 0 failures; exit 0.
- `baseline-model.log`: first `python3 tests/epoll-abi-host/run.py --mode model --source tests/epoll-abi-host/evidence/baseline-syscalls.c.txt`; exit 1, 99 checks, 37 failures. Mock syscall dependencies; NOT a 9os boot/runtime result. The compiler excludes unused-parameter warnings because the baseline ignores `sz` (the bug under test).

No guest, kernel build, non-x86 build, VM, shared fixture, or historical experiment was run or changed. These are independent new ABI tests; they do not address cold startup or release any integration hold.

## Fix and first extended-test attempt

- `fixed-model.log`: original 99-check model against the production fix; exit 0, 0 failures. This is the original test fixture from checkpoint `f5d3b1b` (before extra cases), with the syscall-section SHA printed in the log.
- `linux-extended-dynamic-ubsan-failed.log` and `fixed-extended-model-dynamic-ubsan-failed.log`: first extended fixtures compiled successfully but exited **127 before main** because `libubsan.so.1` is missing. No checks ran; these are infrastructure failures, not positive conformance evidence. `run-dynamic-ubsan.py.txt` preserves the exact runner used. Compiler lookup also returned literal `libubsan.a` and `ls` could not find it, so no static sanitizer retry/install was attempted. Source fixture and production contents are preserved in the checkpoint containing these logs.
