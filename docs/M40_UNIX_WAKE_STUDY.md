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
