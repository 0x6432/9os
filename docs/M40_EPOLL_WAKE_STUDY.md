# Direct epoll wake measurement — no kernel change

The parent is PR38 runtime e0bc041024445770aad61fa70824527b0395a58f.
`sys_epoll_pwait()` still uses unfiltered `poll_wait_seq()` in anonfd.c.
This is a concrete broadcast path, not a demonstrated cause of cold startup
failure. The fixed PR38 cold plan failed; no new cold samples are authorized by
this measurement. PR18 and the dependent integration stack remain held.

`pollwakebench` retains its four original poll modes and adds `--epoll`,
`--epoll-inotify`, `--epoll-unix`, and `--epoll-unix-inotify`. Each new mode
uses 24 private epoll instances and direct `epoll_pwait`, with an eventfd and
optionally an idle inotify descriptor per instance. Exactly 1000 eight-byte
unrelated eventfd or Unix stream transfers run before waking/joining the private
waiters. Returned count, event mask, delivery tag and eventfd value are checked.
There is no poll fallback, extra sample, timing threshold or deadline change;
the original 45-second alarm and phase diagnostics remain.

The ready counter plus historical 20ms settling interval is NOT proof that all
waiters have enrolled in the kernel. Waiter context switches span the thread's
lifetime, including startup and the final intentional wake, not just transfers.
Wall time and context switches remain informational. A sample cannot establish
a statistical rate, a missed wake, desktop causality or reliable GUI startup.

Native x86 Linux controls run all eight modes once, assert mode/output identity,
reject seven invalid CLI forms, and require independent wrong-tag/wrong-count
mutations to fail. Existing exact unixpolltest and its CLI control remain.
Source and signed-prebuilt ALL3 CI, root/UID1000, x86 SMP8 and source MLFQ are
wired to the new modes. Pending GitHub execution is not validation.

Before any kernel candidate: preserve/review the remaining original cold
artifacts and source logs; record an unchanged-kernel guest measurement with
frozen fixture hashes. Any narrowing would need conservative nested/unknown
fallback, epoll_ctl/global topology wakes, fd lifetime ownership, aliases,
ET/ONESHOT behavior, signals and epoch enrollment protection. Existing poll_leaf
annotations are not missing functionality. No scheduler policy, desktop gate,
input injection or diagnostic observer is changed here. M40 is unfinished.
