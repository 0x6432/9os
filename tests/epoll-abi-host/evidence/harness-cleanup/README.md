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
