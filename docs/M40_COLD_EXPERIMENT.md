# Fixed selected-map diagnostic plan — NEVER MERGE this workflow

Prepared but NOT dispatched. Product runtime is pinned exactly to draftPR37
1bb8bc3c538dae207bcc5264d0fe9edb4d7a4345, stacked on409ecfc/PR36.
No kernel/application/readiness/input/deadline changes from PR36; the sole
runtime change is bounded failure-only PC/SP map selection after failure is
irrevocable. Baseline39050199421 FAILED2/3 accepted; paired candidate39053663726
first guestFAILED8/18 and remaining samples pending when this plan was prepared.
Those outcomes stay separate; this plan cannot retry or reclassify them.

Do NOT dispatch before the entire paired series completes and PR37 current-head
fast ALL3 CI succeeds. Full-source gates and all cold/integration holds remain
independent. Exactly one signed Linux6.12.113-r0 control plus3 predeclared fresh
9os boots. Both kernels get exact2-host-logical-CPU affinity from recorded allowed
set, guestSMP4/3072M unchanged, no host-memory cap. Frozen host profile and
before/after fixture hashes. No reruns, retry-until-pass/fail, focus injection,
observer fallback or acceptance waiver.

Unchanged18 physical gates: genuineUID1000PTY, authenticated live-dbus focused/
showing/visible terminal, raw390events/262edges/128taps with zeroerrors, real
Dolphin filename/pixels, cleanexit/lockdep0. Linux alone has sole expected
absent9oslockdep and must pass17 applicable gates before ANY guest. Native and
signed-musl tiny protocol/snapshot controls and desktop33/33 run BEFORE freezing.
Signed locked prebuilts supply Qt/Mesa/LLVM/GLib; no library builds. x86only
here; nonx86onlyPRCI. Normal Qt/Wayland input observersOFF.

Same three failure-only single-thread snapshots with4-second outer bounds,
no unwinding or startup cure. If all guests pass there may be no failed-GUI
selected-map observation; do NOT add samples to chase one. Missing/malformed/
truncated maps and unfinished hashes fail closed. Each guest runs ONCE regardless
of priorguestfailure (after accepted Linux), independent disposable disk snapshot
and immediate always-upload evidence. Any failed sample makes jobFAILURE; no
continue-on-error. Retain whole logs/results/manifests/helpers, no disk/secrets.
NO PR for this branch; its manual-only replacement workflow must NEVER enter
product. M40 and all parent holds remain open even if the whole plan passes.
