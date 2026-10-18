# Separate changed-policy x86 core-only guest validation

Current development head f7b59c1 (full SHA in head.txt). Exactly one predeclared SMP4/512M RR VM. Runtime source differs from the preserved509c26c baseline only by the signal.c no_restart epoll exclusions; CI wiring has no runtime effect.

Actual controller exit0, 6/6 gates (boot plus five commands). epollrestarttest72/72 as root and UID1000; epollabitest78/78 as root and UID1000; lockdepzero. Root filesystem manifest, both consumer hashes, and driver hash exactly match the baseline. Candidate fixture hashes before/after match. The baseline's five SA_RESTART failures per user remain failures on the evidence branch; this changed-head run is not a retry or retroactive rescue.

Original complete logs, build output, pinned head, predeclared plan, fixture hashes and root manifest retained. No deadline, test consumer, scheduler or enrollment change. No VM overlap or active fixture mutation.

Core-only x86 proof, not graphics, non-x86 or startup reliability proof. Source/signed-prebuilt current-head GitHub CI remains outstanding. PR40 cold FAILED and all integration holds remain unchanged.
