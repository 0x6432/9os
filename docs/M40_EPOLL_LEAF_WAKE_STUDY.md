# WIP: direct leaf-only epoll wait filtering

Parent PR39/f2aa0ab adds measurement only. Its ordinary signed-prebuilt ALL6
passes were reviewed; source validation remains separate. The frozen x86
unchanged-kernel baseline is retained on the evidence-only branch under
m40-epoll-wake-baseline. This candidate is a bounded broadcast-reduction
hypothesis, NOT a demonstrated cold-start fix. All integration holds remain.

## Kernel change

During an epoll scan, collect inode Bloom keys ONLY while item_get() pins the
file and the epoll instance mutex protects its registration. Store only integer
keys in the scan context; no file/inode pointer is carried into the wait.
Known existing poll_leaf sources contribute keys. Any unknown or nested source
forces a sticky UINT64_MAX/global fallback. Empty or all-disabled lists also
wait globally. No new leaf annotations or scheduler policy changes.

Direct waits use the existing poll_wait_seq_masked() epoch/enrollment protocol,
with the epoch read BEFORE scanning. Every successful epoll_ctl ADD/DEL/MOD
still notifies globally, including changes between scan and enrollment.
All existing producer/release notifications are unchanged. fd_close does NOT
in general issue a global notification: this candidate does not add that
promise. The existing per-caller fd identity/stale-item pruning and registration
lifetime semantics are unchanged, not upgraded to full Linux OFD semantics.
ET eligibility, ONESHOT delivery/rearm, BKL handling, nesting bounds, deadlines,
signal masks and EINTR cleanup are unchanged. Filter collisions add wakes only.

## Exact controls

Expanded pollfiltertest retains its original poll/ppoll/select/blocking-read
checks and adds direct/mixed/nested epoll alias delivery, original-descriptor
close, timeout then select, public inotify leaf delivery, threaded ready-fd
ADD/MOD, DEL, ONESHOT rearm, dynamic ET drain/refill, and epoll_pwait signal-mask
restoration/EINTR. The original 30-second process alarm is unchanged.

Expanded unixpolltest retains all original paths and adds direct epoll for
dup-only STREAM/SEQPACKET/DGRAM receivers, unrelated private eventfd unready,
full-buffer duplicated writers, peer shutdown/full-close EOF/RDHUP/HUP.
The original 40-second alarm and two-second readiness bounds are unchanged.

The exact production scanner is compiled with mocked locks/FD pins: 15 groups
cover key union/aliases, conservative fallbacks in either order, stale slots,
disabled/empty lists, nested peek without ONESHOT consumption, ET/ONESHOT state,
maxevents and BKL balance. Eight independent mutations must assert, including
key-after-release. These mocks are NOT SMP/lifetime sanitizer or GUI proof.
The original exact epoch/filter model and its seven mutants are retained.

ALL3 source/signed-prebuilt root/UID1000 CI already invokes both consumers;
source MLFQ and source x86 SMP8 now also invoke pollfiltertest root/UID1000.
Only x86 is built/tested locally; no Qt/Mesa/LLVM/GLib library is built locally.
Local core tests are not full graphics regression. Current-head ALL3 CI and any
separately predeclared cold comparison remain required before integration.
No new cold samples, retries, observer fallback or acceptance rescue here.
M40 remains unfinished; evidence/manual-validation branches must never merge.
