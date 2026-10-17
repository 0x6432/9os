# Validation-only durable cold experiment — never merge this branch wholesale

This branch is based on PR36 candidate runtime commit
`409ecfc018c72cb65ac91cdd4481e265ab0b2377`. It replaces the existing Alpine workflow
ONLY ON THIS NON-PR VALIDATION BRANCH with a manual x86-only experiment. Never
merge this workflow over the product workflow. It changes no kernel, userland,
readiness deadline, sender, application or acceptance gate. The workflow asserts
that runtime diff is empty. Pushing this non-main branch without opening a PR
does not restart PR35's expensive source workflow.

Plan: unchanged desktop prerequisite, then exactly one signed Linux6.12.113-r0
control and THREE separately planned fresh 9os cold boots. All guest boots are blocked unless Linux passes
all17 applicable gates, with the missing9os lockdep gate explicitly retained as
the sole failure. 9os must pass all18 gates plus raw physical input capture. Each
sample needs both root/user snapshot selftests, genuine focused-terminal witness,
explicit live-dbus backend and immutable before/after hashes. Qt/Wayland input
observers stay off; failure-only register snapshots remain enabled. No fallback,
waiver or retry. A rerun attempt is rejected. A failed/unknown result remains
failed/unknown even if a later independent CI job passes.

Locked signed APKs supply Qt/Mesa/LLVM/GLib. Only kernel and tiny consumers are
compiled. Signed reference Linux modules are installed with package scripts
disabled. Both kernels use the same frozen disk and physical-key driver under
TCG; each gets a disposable snapshot. Build-free adapters prevent make from
silently repacking ISO/initramfs after hashes are frozen. The raw-input tiny
observer is installed before freezing, not into an active root.

Artifacts are uploaded after the desktop prerequisite and after EACH sample,
including failures. Complete serial logs (including genuine PNGs), results,
original manifests, exact scripts, source/lock metadata and failure diagnostics
are retained in GitHub even if the interactive sandbox disappears. No disk
image, credentials, private helper or entire desktop library tree is uploaded.
The service-owned CI log is an additional durable record. If a timeout occurs
before final hashes/results, acceptance remains false/unknown; no guest follows
an invalid control.

Host controls exercise exact positive shapes and reject missing/stale/echoed
witnesses, duplicates, extra failures, different Linux failures, invalid raw
capture, observer flags and changed fixture integrity. These synthetic controls
are NOT boot or GUI evidence. Current-head functional PR CI already passed all3
33/33 desktop +16/16 physical; full-source PR CI and cold-reliability holds are
separate. Genuine earlier missing-PTY and focus failures are not erased. M40 is
NOT complete. The previous local physical experiment lost to a sandbox reset has
UNKNOWN outcomes and is not reconstructed or silently replaced by this new job.


## Three-sample plan, not a retry of the original plan
Original fixed run39035386400 at58f26eb completed Linux17/18 applicable and
9os18/18 with both fixture hashes and raw capture accepted. Evidence commit
dfb8228 preserves the complete original artifacts; no outcome is overwritten.
This new branch leaves the original validation branch unchanged and keeps the
runtime935bd74 identical. It defines guest1/guest2/guest3 BEFORE execution.
After an accepted Linux control each guest runs exactly once, regardless of an
earlier guest failure, so any failure diagnostics are retained without selecting
only successes. A failed step still makes the job fail. No continue-on-error,
retry, observer fallback, deadline enlargement or changed acceptance threshold.
Each guest has its own result/log/PNG and immediate always-upload artifact.
Linux failure or cancellation blocks every guest. Separate snapshot overlays
prevent persistent effects; fixture hashes are checked before/after EACH sample.
A three-sample success is bounded repeated functional evidence, not a causal fix,
statistical reliability guarantee, approval to merge held work, or M40 completion.


## New predeclared two-logical-host-CPU profile
Prior fixed-three run39036408360 at9d84d00 passed all3 guest18/18, with control,
raw events and frozen hashes accepted; original complete evidence is retained
atc518950. It does not reproduce the smaller sandbox's failures or prove a cure.
This NEW validation branch uses the exact same935bd74 target runtime and same
oneLinux+threeguest plan, but freezes the first TWO allowed logical host CPUs
and applies that affinity to the entire driver/QEMU subprocess tree for BOTH
kernels. Python verifies the applied affinity and records it in each result.
Fewer than2 allowed CPUs or invalid affinity rejects without fallback. Guest
SMP4 and3072M, all gates/timeouts/observers/keyboard path stay unchanged. Host
CPU availability, memory and cgroup CPU quota are recorded before the experiment
and hashed. No host memory limit, physical-core guarantee or scheduler-runtime
fix is claimed. This is a host-constraint hypothesis test, NOT rerunning a failed
sample, overwriting earlier results, selecting successes or waiving any hold.


## Pipeline candidate: new fixed comparison, not a failed-baseline retry
The original two-CPU baseline39050199421 at99f9d60 retains firstguest8/18 failure
withvalidhashes, realPTY but timed-out focus query. Guest2 separatelypassed18/18;
lastsamplepending when this new plan was prepared. All baseline outcomes remain.
This validation branch now pins EXACT PR36 candidate409ecfc runtime. It changes
only each node's live read-only GetRole/GetChildren request scheduling, not the
kernel, applications, PID/role/state acceptance, any deadline, focus or physical
input path. New27-case native+signed-musl real private-bus controls must pass
before booting. Same frozen actualtwohostCPUs, oneLinuxcontrol then3predeclared
freshguest samples, per-sample artifacts and noretries. This separate plan must
not run until the original fixed baseline series has completed. No selecting
successes, comparing unknown outcomes, retroactive cure or M40 completion claim.
