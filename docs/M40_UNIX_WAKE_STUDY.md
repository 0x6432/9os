# WIP: connected Unix data-plane poll wake study

The first measurement commit (c63d67a) changes no kernel behavior. The later
candidate (0babfb5) narrows connected data wakes, as described below. Neither
changes a readiness deadline or desktop gate.
The existing informational `pollwakebench` now also supports `--unix` and
`--unix-inotify`: exactly1000 eight-byte stream socketpair write/read transfers
while24 unrelated private eventfd pollers sleep (optionally mixed with inotify).
All original eventfd modes and data/readiness/join checks remain. Same45-second
progress alarm with explicit blocking-phase failure; wall-time/context-switch
counts remain informational, never correctness/performance thresholds.

Baseline source review: AF_UNIX send/receive call global `poll_notify()`, which
wakes ALL poll waiters despite existing per-leaf poll filters. This is a concrete
broadcast mechanism, NOT a demonstrated cause of the M40 cold failures. Measure
unchanged9os first; do not infer benefit from Linux or implement a speculative
kernel cure. Any later narrowing must preserve stream/seqpacket/dgram, backlogs,
blocking I/O, aliases, descriptor passing, shutdown/close, select/nestedepoll,
signals, full-buffer writable transitions and inode/file lifetime ownership.
Unknown/many-to-one readiness dependencies require conservative global wakeups.

Native x86 Linux tiny consumer: four modes pass with zero data/readiness errors;
four invalid CLI forms reject. StrictC and diffcheck pass. No local desktop
library builds/nonx86 tests. The restored core-only x86 results are recorded
below; they are not full graphics regression results. No merge or desktop/
reliability claim.
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
This candidate still requires current-head ALL3 PRCI before integration. No
merge, M40 cure or reliableGUIclaim. No active fixtures were modified.

The first candidate's ONE fixed x86SMP4/512M VM passed16/16, including all six
benchmark commands, root/user pollfiltertest/socktest/ipctest/polltest andlockdep0;
allfrozenhashes valid. Unix-transfer privatepoller contextswitches reduced in
these samples, but magnitudes vary and there is NO claimed statisticalrate,
desktop causalfix or M40acceptance. Kernelcode remains unvalidated
on nonx86 until PRCI. No failedtest was retried or reset.

New exact-source unixpolltest passes216 nativeLinux checks (count can vary with
buffercapacity). It covers stream/seqpacket/dgram dup-only receiver poll/ppoll,
nestedepoll, blockingreads, full-buffer duplicatedwriter POLLOUT after drain,
and peerhalf/fullclose EOF/RDHUP/HUP. Native CLI/installer missing+duplicate
controls pass. All originaltests retained, newroot/user tests/benchmark modes
added to source/signedALL3CI and x86SMP8, withsourceMLFQ coverage. Guest results
for this NEW test are recorded below. No desktop library builds or localnonx86.

## Fixed new-test guest validation at 3a90eed
One predeclared sequential x86 VM each SMP1/SMP4/SMP8, 512M core-only, two
selected host CPUs, identical frozen image/binaries. Each accepted18/18,
including unixpolltest274checks0fail as root AND UID1000, all six benchmark
commands and original root/user socket/IPC/poll regressions, boot andlockdep0.
Every before/after fixture hash matched; no retry or overlap. Complete original
evidence is on the evidence-only branch under m40-unix-wake-conformance.
Review confirmed indirect select/epoll waits remain unfiltered, all control
paths global, only connected data enqueue/drain narrowed with both keys,
key capture/publication protected by unix_lock, no pointer escapes unlock.
Current-head ALL3 CI, complete graphics regression and a separately fixed cold
comparison remain outstanding. This is NOT a GUI cure or M40 completion.
