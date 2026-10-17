# Validation-only durable cold experiment — never merge this branch wholesale

This branch is based on PR35 runtime commit
`935bd746cb6ea6d9ed833a5125b8f4465dc31ee7`. It replaces the existing Alpine workflow
ONLY ON THIS NON-PR VALIDATION BRANCH with a manual x86-only experiment. Never
merge this workflow over the product workflow. It changes no kernel, userland,
readiness deadline, sender, application or acceptance gate. The workflow asserts
that runtime diff is empty. Pushing this non-main branch without opening a PR
does not restart PR35's expensive source workflow.

Plan: unchanged desktop prerequisite, then exactly one signed Linux6.12.113-r0
control and one fresh 9os cold boot. Guest boot is blocked unless Linux passes
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
