# Real seized job-stop signal conformance

Exact expanded portable ptracestoptest: native x86 Linux zero failures; original
9os fails only new SIGSTOP-vs-SIGTRAP assertion as root and UID1000 (2/4).
Candidate reports the still-active group stop via ptrace_group_stop; running
interrupts retain SIGTRAP. Candidate x86 passes5/5, including same expanded
root728/user1520 checks, existing ptracetest and lockdep0. All frozen fixture
checks passed before/after each stopped VM. No local non-x86 builds/tests.

Old PR35 all3 desktop failures and complete CI logs are retained. They caught
the same stop-signal mismatch; no test weakened to accept it. Non-setuid target
also corrects a separate Linux fixture issue; it changes no security policy.

The earlier local Linux physical control failed5/18 at unprivileged attachment
to setuid-BusyBox synthetic tracees. It blocked the dependent guest trial. Its
complete local archive was LOST in sandbox reset before durable upload; do not
claim it is in this ZIP or accepted. Earlier genuine bcdf360 missing-PTY failure
remains in separate evidence commit aeeaeb963896d978f3f770f962f1af782253f639.

This is NOT a Qt startup cause, reliability cure, physical-input proof or M40
completion. Current-head all3 CI and integration holds remain. Evidence-only
branch: never merge binary archives into product. ISO/rootfs hashes are retained,
not whole disk images. Baseline executable/kernel, exact source diff, all relevant
native/guest logs and old failed CI logs are included.
