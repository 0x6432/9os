# Frozen failure-only diagnostic trials, 2026-10-17

This is an evidence-retention branch only; do not merge binary archives into
the product branch. It preserves complete stopped-VM logs/checkers/scripts and
fixture hashes because the session-file download helper failed in this sandbox.
No credential helper or token is included; all archive members were scanned.

Runtime: PR32 f9609c49101240650a3110992b2d2f2a9c1ee835, with a PRIVATE
failure-only probe addition. The original focus/readiness decision and deadlines
are unchanged and irrevocable before diagnostics execute. The added normal
libatspi phase trace and separate traced attempt cannot rescue a failed boot.
No Qt/Mesa/LLVM libraries were compiled locally. x86_64 only locally; the
signed Linux 6.12.113 control used matching QEMU resources and a disposable
snapshot of the same disk. All original physical gates and input taps retained.

See results.json and each complete archive, including failures. Linux lacks
9os /proc/lockdep: its exact missing-lockdep failure is not hidden or counted as
9os acceptance. Raw/outer/Qt checks establish only their named layer, not
application consumption or unchanged-timing reliability. Failed readiness means
no physical typing was authorized; an empty capture is NOT keyboard proof.

The first 9os failure-only phase trace shows successful query progress but
termination at the eight-second boundary before terminal state observation.
This is not proof of an unfocused terminal, nor proof of a missed kernel wakeup.
The separate bulk-query prototype is NOT included in these runtime fixtures.
M40 and all existing cold-start/typing/integration holds remain open.
