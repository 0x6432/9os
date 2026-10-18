# One pre-policy-fix x86 core-only guest baseline

Product development snapshot 509c26ca088d219c078f45d2aaa60ae395af3667. Exactly one predeclared SMP4/512M RR VM; no repeats or acceptance rescue. Consumer from agent test-only commit8bef592 unchanged, before signal.c policy fix.

Actual controller exit1. epollrestarttest failed5/72 checks as root and again5/72 as UID1000: every caught-SA_RESTART case returned0 instead of-EINTR. The finite100ms waits completed; these are not infinite-wait hangs. All non-SA_RESTART and mask assertions passed. epollabitest78/78 as root and UID1000. Lockdepzero accepted. Frozen fixture before/after SHA256 files match exactly.

Original complete guest/controller/build logs, test pick, pinned head, predeclared plan, root manifest, driver and fixture hashes retained. A development cherry-pick initially stopped for missing committer identity before any runtime; it was continued using the required identity. No VM retry occurred.

Core restoration only. No graphics, startup causal or non-x86 validation claim. PR40 cold FAILED and all integration holds remain.
