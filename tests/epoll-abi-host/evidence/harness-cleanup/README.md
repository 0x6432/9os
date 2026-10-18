# Native harness descendant-cleanup defect (R1)

Independent reviewer report:
`/data/team-coordination/epoll-consumer-independent-review.md`.
At source checkpoint `d7ef6647d913cb34ec12900cf0d64fa39d7654d4`,
`guest-watchdog-control.py` used `subprocess.run(timeout=6)`, which killed/reaped
only its direct supervisor on exceptional timeout, not the signal-blocked child.
The original defective script is preserved here as
`original-guest-watchdog-control.py.txt`.

`independent-first-stall-failure.log` preserves the reviewer's first intentional
supervisor-stall observation: the six-second outer timeout left its child alive.
The independent wrapper then group-killed/reaped the child. This is a genuine
harness cleanup gap, not a guest/kernel failure, and not a reason to relabel the
previous normal five-second watchdog control pass as false.

The reviewer's exact temporary native source remains at
`/data/team-coordination/epoll-consumer-review-controls/watchdog-supervisor-stall-control.c`.
No original evidence or independent review files were modified.

Repair scope is native harness/docs/dedicated controls only. Preserve the
consumer's five-second deadline, outer six-second failure, original failures,
and all runtime-consumer/kernel sources. No VM/heavy/non-x86/auth/push work.

## Fixed native harness controls

`isolated_process.py` launches each control in a new session/process group using
Popen. For that single-threaded execution it temporarily enables Linux child
subreaper mode, restoring the prior setting afterward. On **any exception** it
SIGKILLs only the isolated group, collects/reaps the direct supervisor, reaps
adopted descendants in that group, and re-raises the original exception. It does
not retry a control or convert TimeoutExpired into acceptance. Normal returns
also reject leftover adopted/group processes. This is controlled-test ownership,
not containment of adversarial descendants that deliberately escape their group.

First repaired-harness observations:

- `fixed-five-second-control.log`: `python3 -B tests/epoll-abi-host/guest-watchdog-control.py`; harness exit 0. The unchanged child-hang fixture returned the expected consumer exit **124** at **5.002170s**. The exact consumer and temporary mutation hashes match the earlier valid normal control.
- `fixed-supervisor-stall-control.log`: `python3 -B tests/epoll-abi-host/test-guest-watchdog-cleanup.py`; regression harness exit 0. The child blocks signals/hangs, and the supervisor is intentionally SIGSTOPped before its original watchdog loop. **TimeoutExpired(timeout=6)** remained the result at **6.007193s**, observed only after supervisor SIGKILL and adopted-child SIGKILL/reaping. The isolated group and both PIDs were absent; the child was already reaped; an unrelated live child was untouched; prior subreaper state was restored.

The regression harness catches that expected failure only to assert cleanup; the
production native-control runner still propagates it as failure. The consumer's
five-second threshold and native runner's six-second timeout are unchanged.
No consumer/kernel/CI/shared-workflow source was modified, and there was no
kernel/guest/non-x86/heavy/sanitizer run. The inherited SA_RESTART policy work is
separate and untouched. Baseline37/99, sanitizer-unavailable and all original
native/model/normal-watchdog evidence remain preserved.
