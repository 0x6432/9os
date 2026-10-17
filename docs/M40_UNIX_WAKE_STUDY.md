# WIP: measure Unix data-plane poll broadcast before any kernel change

This branch changes NO kernel behavior, readiness deadline or desktop gate.
The existing informational `pollwakebench` now also supports `--unix` and
`--unix-inotify`: exactly1000 eight-byte stream socketpair write/read transfers
while24 unrelated private eventfd pollers sleep (optionally mixed with inotify).
All original eventfd modes and data/readiness/join checks remain. Same45-second
progress alarm with explicit blocking-phase failure; wall-time/context-switch
counts remain informational, never correctness/performance thresholds.

Source review: AF_UNIX send/receive currently call global `poll_notify()`, which
wakes ALL poll waiters despite existing per-leaf poll filters. This is a concrete
broadcast mechanism, NOT a demonstrated cause of the M40 cold failures. Measure
unchanged9os first; do not infer benefit from Linux or implement a speculative
kernel cure. Any later narrowing must preserve stream/seqpacket/dgram, backlogs,
blocking I/O, aliases, descriptor passing, shutdown/close, select/nestedepoll,
signals, full-buffer writable transitions and inode/file lifetime ownership.
Unknown/many-to-one readiness dependencies require conservative global wakeups.

Native x86 Linux tiny consumer: four modes pass with zero data/readiness errors;
four invalid CLI forms reject. StrictC and diffcheck pass. No local desktop
library builds/nonx86 tests, no guest benchmark result yet. Core-only x86 setup
is being restored after sandboxwipe. No PR/merge or desktop/reliability claim.
Originalserial2/3, paired0/3 and selected-map1/3 accepted cold series remain
separate, all failures and holds retained. Six selected failure snapshots now
cover actual code/stack mappings but are post-failure/timing-perturbed; they do
not prove a fixed deadlock. M40 remains unfinished.

## WIP candidate after the single frozen baseline
Baseline x86 core8/8 accepted, both fixture hashes valid; exactlogs retained in
evidence-only ec0c82b. No causal desktopclaim. Candidate narrows ONLY connected
stream/seqpacket enqueue/drain notifications to BOTH stable endpoint Bloomkeys.
Key publication/clear is under unix_lock; no file/inode pointer survives unlock.
Datagrams, missing/unaccepted endpoints and EVERY control-plane/backlog/close/
shutdown path remain global; unfiltered blocking/select/epoll still wake on
any endpointnotification. Epoch enrollment and existing eventfd behavior retained.
This code is UNVALIDATED until actual x86 tests, full review and ALL3 PRCI. No PR,
merge, M40 cure or reliableGUIclaim. No active fixtures were modified.

The first candidate's ONE fixed x86SMP4/512M VM passed16/16, including all six
benchmark commands, root/user pollfiltertest/socktest/ipctest/polltest andlockdep0;
allfrozenhashes valid. Unix-transfer privatepoller contextswitches reduced in
these samples, but magnitudes vary and there is NO claimed statisticalrate,
desktop causalfix or M40acceptance. Kernelcode remains unreviewed/unvalidated
on nonx86 until PRCI. No failedtest was retried or reset.

New exact-source unixpolltest passes216 nativeLinux checks (count can vary with
buffercapacity). It covers stream/seqpacket/dgram dup-only receiver poll/ppoll,
nestedepoll, blockingreads, full-buffer duplicatedwriter POLLOUT after drain,
and peerhalf/fullclose EOF/RDHUP/HUP. Native CLI/installer missing+duplicate
controls pass. All originaltests retained, newroot/user tests/benchmark modes
added to source/signedALL3CI and x86SMP8, withsourceMLFQ coverage. Guest results
for this NEW test are still pending. No desktop library builds or localnonx86.
