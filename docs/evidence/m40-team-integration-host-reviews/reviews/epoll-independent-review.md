# Independent epoll ABI review — pinned 18ef21f → 6f0fcef

## Verdict

**No blocking defect introduced by the reviewed `kernel/fs/anonfd.c` diff was identified.** The timespec validation, nanosecond arithmetic, finite saturation, and sigset-size ordering are internally consistent and agree with the exercised Linux reference cases. This is a scoped code-review verdict, **not approval to merge, a guest pass, or a release of any integration hold**.

**Important inherited ABI finding:** epoll waits are missing from the kernel's `SA_RESTART` exclusion policy. The new direct-syscall model does not exercise the signal-return path, so its EINTR checks cannot detect this. This predates the patch and should be separately adjudicated by the parent; do not silently broaden the epoll agent's ownership into signal code.

Reviewed base: `18ef21fc34945f482d262993aa33690070aa86d5`.
Reviewed head: `6f0fcef3d07d62497082d274ea4da56318790dc9`.
Worktree: `/data/9os-team-epoll`, read-only. The concurrently developed guest consumer is **outside this pinned review**.

## Findings and actions

### P1 — inherited signal-restart ABI mismatch; not introduced by this diff

**Evidence:**

- `kernel/core/signal.c:14–21`, identical at base and reviewed head, excludes nanosleep/ppoll/pselect/sigsuspend/etc. from handler-driven restart, but includes none of `__NR_epoll_wait`, `__NR_epoll_pwait`, or `__NR_epoll_pwait2`.
- `kernel/core/signal.c:371–376` restarts an interrupted syscall with a caught `SA_RESTART` handler when `!no_restart(nr)`.
- On x86, `kernel/arch/x86_64/include/arch/syscall.h:29` sets `rax=nr` and rewinds `rip` by two bytes. This is an actual reissue, not merely retaining an internal EINTR flag.
- `kernel/fs/anonfd.c:540,546` returns EINTR/deferred-mask state, but userspace return subsequently traverses the above signal path. Reissuing a finite wait creates a fresh relative deadline; reissuing the huge/infinite cases can continue waiting after a one-shot alarm handler.
- The existing model calls the syscall bodies directly. Its mocked wait returns EINTR (`model-pre.c:64–70`), and assertions stop at `saved_mask`/`restore_mask` (`model-post.c:42–51`); neither `signal_deliver`, frame construction, nor sigreturn executes.
- Independent Linux rerun with **explicit** `sigaction(..., SA_RESTART)` in an outside-worktree copy of the native fixture still returned EINTR for all six interruption cases and passed all76 checks. Log: `/data/epoll-independent-review-run/explicit-sa-restart.log`.

**Impact/qualification:** high-confidence static inherited correctness issue, **not reproduced in a 9os guest in this review**. A direct-model EINTR pass must not be described as complete Linux signal ABI conformance. The long one-shot-alarm cases could hit the outer watchdog rather than return if reused as guest tests with SA_RESTART enabled.

**Action:** parent should track/fix the exclusion policy in an explicitly owned follow-up, or explicitly retain it as an unresolved ABI limit. Add a real guest case with a caught signal and explicit SA_RESTART, both with and without a temporary mask, that expects EINTR and original mask restoration after sigreturn. Start with a bounded finite wait (e.g.100ms wait /30ms one-shot signal) so a bad restart returns the wrong value instead of waiting indefinitely; do not weaken the expected EINTR contract. Test SA_RESTART off as a separate control. Guest execution remains subject to the parent's resource grant.

### P2 — model verifies forwarding, not real epoch/filter enrollment (coverage gap)

**Evidence:** `model-pre.c:55–70` increments a synthetic sequence each read, always supplies filter0x42, and immediately returns a selected wake/timeout/EINTR. `model-post.c:43–47` therefore proves deadline retention, filter argument forwarding, and a new epoch sample, **not** a concurrent epoch mismatch or race-free real scheduler enrollment. The real implementation is `kernel/fs/vfs.c:61–69` and `kernel/core/sched.c:730–767`; these are not part of the extracted model.

The actual scan/filter construction, zero-filter/global fallback, nested/unknown-source sticky fallback, disabled ONESHOT handling, and ET edge state are also mocked away. They are unchanged by the patch, so this is **not evidence of a new filter regression**.

**Action:** preserve the existing claims' narrow scope. Low-cost future model controls can exercise a no-enrollment epoch-mismatch rescan, scan-without-BKL, and deadline expiry during a scan. Actual guest controls should cover direct-leaf wake, empty/all-disabled/global fallback, nested epoll, ET/ONESHOT, and a producer update at enrollment. Reuse existing focused controls where possible rather than treating the seven model mutations as concurrent runtime proof.

### P2 — actual guest signal/mask and target validation still absent at pinned head

Native Linux establishes expected behavior; extracted production syscall bodies establish arithmetic/control-flow behavior under mocks. Neither establishes that 9os delivers a signal under the temporary mask, restores the old mask through a real signal frame/sigreturn, or wakes correctly on the target scheduler. No guest or non-x86 result is present in the reviewed commit. Dynamic-UBSan startup failures are correctly retained as failures, not sanitizer passes.

**Action:** review and run the epoll agent's later guest consumer under an authorized validation path; include ready, timeout, EINTR, EFAULT cleanup and mask restoration, finite huge versus infinite, and existing wake/filter regressions. Require new-head integration CI. These are outstanding validation conditions, not permission for a local VM/heavy build now.

## Arithmetic, ordering, and unchanged-path audit

- `anonfd.c:562–565`: timespec copy precedes mask/FD/max checks; negative seconds/nanoseconds and nanoseconds >=1e9 reject before arithmetic. Shared native/model cases cover invalid time even when ready, time-copy versus mask-size, mask-size versus bad pointer/maxevents/fd, and null-mask size ignoring.
- `anonfd.c:564–565`: after validation, unsigned division tests the multiplication/addition horizon before calculating it. Saturation yields `INT64_MAX`, never `UINT64_MAX`. No signed `int` millisecond intermediate remains.
- `anonfd.c:551–553`: every negative int-millisecond timeout remains infinite; nonnegative `INT_MAX *1e6` fits safely.
- `anonfd.c:521–525`: `now >= INT64_MAX` short-circuits before the subtraction; otherwise the remaining-horizon comparison prevents deadline overflow. Finite saturation and the infinite sentinel are distinct. This intentionally uses a signed-nanosecond horizon, not an assertion about behavior after centuries of uptime.
- `anonfd.c:527–540`: original deadline survives rescans; scan time is deducted before enrollment. Epoch sampling precedes scan and the unchanged filter goes to the unchanged masked wait. A timeout/spurious wake rescans; EINTR leaves the loop.
- `anonfd.c:519–520,542–547`: mask stripping, allocation/ref lifetime, copyout failure cleanup, normal restoration and deferred EINTR restoration remain unchanged. New size/time validation occurs before mask installation. **Deferred flags alone do not settle the inherited SA_RESTART issue above.**
- Actual `ep_scan_locked`, `ep_wait_filter`, CTL/ET/ONESHOT bookkeeping, file pinning, scheduler/wait implementation, and signal implementation have no modifications in this diff.
- Pre-existing maxevents4096 limit and other broad epoll conformance issues remain outside the patch. No complete-ABI claim is warranted.

## Independently rerun evidence

Pinned files were copied with `git show 6f0fcef:<path>` into `/data/epoll-independent-review-run`, so running tests did not edit or depend on concurrent uncommitted guest work.

Commands:

```
python3 /data/epoll-independent-review-run/tests/epoll-abi-host/run.py --mode native
python3 /data/epoll-independent-review-run/tests/epoll-abi-host/run.py --mode model --mutations
```

Results:

- Linux6.18.49 x86_64, GCC11.5.0.
- Native Linux reference: **76 checks, 0 failures; exit0**.
- Exact extracted syscall model: **119 checks, 0 failures; exit0**.
- **7/7 mutations rejected by explicit FAIL-check mismatches**: ignore sigset size22; accept billion ns2; truncate milliseconds20; overflow seconds1; lose original deadline6; lose filter2; restore mask too early2.
- Extracted syscall SHA256: `10432fbf63f499757e0f20e269d5894ea9a57e19d2644a794b6eb74f40974feb`.
- Full rerun output: `/data/epoll-independent-review-run/review-rerun.log`.
- Explicit-SA_RESTART native variant: `/data/epoll-independent-review-run/native-explicit-restart.c`; binary compiled only as a tiny native x86 consumer, five-second outer watchdog; **76 checks, 0 failures; exit0**. Output: `/data/epoll-independent-review-run/explicit-sa-restart.log`.

No VMs, kernel builds, non-x86 execution/build, dependency installation, credential access, worktree changes, pushes, PR actions, merges, or hold waivers. PR40cold remains FAILED; M40 and all inherited integration holds remain open.
