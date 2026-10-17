# PR36 fixed two-host-CPU comparison — first guest FAILED

Run https://github.com/0x6432/9os/actions/runs/39053663726 was dispatched ONCE,
after original baseline39050199421 completed FAILED with2/3 accepted guests.
Validation3718e0e pins product409ecfc. Linux control plus3 independent guest
samples were predeclared; no retries, fallback or relaxed acceptance.
Both kernels run with verified host affinity[0,1] from allowed[0,1,2,3], guest
SMP4/3072M unchanged. Host memory16GB: NOT a4GB-memory reproduction.

Linux control accepted17/18 applicable, sole expected absent9oslockdep. Exact
raw390events/262edges/128taps, zero drops/repeats, frozen hashes pass.
Guest1 FAILED8/18, before/after frozen hashes pass, driver1/raw1. Genuine
PTY exists, readiness pty=1/terminal=0 pid911. Paired GetRole/GetChildren are
actually issued before either completion; the bounded query still expires
at a shallow tree node before observing a focused terminal. This does NOT
prove the real terminal was unfocused and does NOT identify a kernel cause.
No physical sender rescue; no valid input/Dolphin acceptance. All3 failure-only
snapshots captured PC7e5e335382e7/SP7fffaca21428, but printed maps256/truncated1;
addresses lie outside printed maps, so no module/frame attribution. Failure-only
bulk query also alarm142. Snapshot timing perturbed AFTER irrevocable failure.

Remainingguest2/3 run once each as predeclared; pending at this checkpoint.
Original complete signed-download artifacts retained here unchanged with SHA256.
PR36 fastALL3 green is separate, observed CI evidence, NOT a cold-start cure.
The candidate has reproduced the held failure. No merge and M40 remains open.

## Second predeclared guest also FAILED8/18
Guest2 artifact12109625920 is retained unchanged. Both fixture hashes valid;
driver1/raw1, genuinePTY1/terminalverification0 pid833, bounded live query
still alarm-expired before terminal observation. Failure-only PC7e5a8ad462e7/
SP7ffefd965e38 again lack map coverage: samples1/3 printed256/truncated; sample2
did not complete map output in its unchanged bound. Failure-only bulkrc142.
No kernel/module/frame attribution or accepted physical result. Guest3 pending,
not an invitation to retry either failure. Candidate has NOT resolved the gate.

## Final predeclared outcome: FAILED,0/3 accepted guest samples
Run39053663726 completed FAILURE. Guest3 also failed8/18 with BOTH hashes valid,
PTY1/terminalverification0 pid868, driver1/raw1. All3 original guest artifacts
are retained unchanged. No retry/reclassification. Linux accepted17/18 only.
Guest3 failure-only query DID later observe the real focused/showing/visible
terminal, authenticated pid868, models1/visited7, rc0 AFTER failure-only ptrace
snapshots. That timing-perturbed post-failure observation CANNOT rescue its
earlier failed readiness/physical gates and does not prove a root cause. Earlier
rawPC7e47bf8a42e7/SP7ffde5122118 remain outside printed truncated256 maps.
PR36 paired queries have NOT resolved the failure. Original serial baseline
2/3 accepted and candidate0/3 are small distinct series, not a statistical
regression proof or general reliability rate. All holds/M40 gaps remain open.

A separately prepared diagnostic-only plan8210a0a pinsPR37 runtime1bb8bc3.
NOT yet dispatched: requires this whole candidate series complete AND current
PR37 fastALL3 success. No new samples added to chase a desired outcome.
