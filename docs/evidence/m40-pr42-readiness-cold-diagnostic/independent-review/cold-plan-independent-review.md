# Independent read-only review — PR42 readiness-history diagnostic cohort

## Verdict and reviewed state

**No plan-changing blocker found for the separately predeclared, one-shot
readiness-history diagnostic question.** The new plan retains the held manual
physical driver/evaluator, exact physical acceptance gates, accepted-Linux
barrier, three independently scheduled guest steps and failure preservation.
This is **not** dispatch authorization, a cold cure, a performance comparison,
a reliability-rate estimate, or release of any PR18→42/M40 hold.

Reviewed `/data/9os-pr42-cold-validation`, branch
`notion/m40-team-readiness-cold-validation`. It initially contained an uncommitted
plan atop runtime `07fda66bd9a7904e77b7f3e1051df243ee1e4d21`. During this read-only
review, an external/parent commit materialized the same scoped plan as
**`ae0ed93cc17743a6d879f089d4aee1cfb217e16e`**. The final worktree is clean. I did
not edit, stage, commit, prepare fixtures, launch samples, dispatch/query CI,
access credentials, push, build a kernel/library, or run a VM/non-x86 test.
Only review files/logs under `/data/pr42-current/` were written.

Comparison baseline: `/data/9os-epoll-leaf-validation`, held manual commit
**`ad39a795cae2ade0083d451cdaf9edea3735ae33`**.

## Required invariants — confirmed by source/host controls

### 1. Held driver and evaluator are byte-identical

Compared both new working files against both the old worktree and `git show`
of the exact held commit:

- `scripts/m40-cold-driver.sh` SHA256:
  `abd83f5b2669f49ea96bf51a30a4671b58607d2daca337c87afcb5c3e6e42df9`
- `scripts/m40-cold-run.py` SHA256:
  `2cd85582703aaa0fd49022edec80ccd8642cbc12c3a5131f3f0d36a8e8cd8e68`

The preparer differs from the held plan only in runtime SHA and PR40→PR42
labels. Its host controls change the expected pin. The workflow changes its
name/comment/concurrency group, runtime pin, and adds
`terminal-attempt-history-host-tests.py` to existing host prerequisites. The plan
document separately declares the new diagnostic scope and preserves the prior
failed cohorts. No hidden physical-driver or acceptance-evaluator relaxation
was found.

### 2. Exact18 gates / sole expected Linux exception

A separate shlex/argparse inspection of the literal driver found **17 explicit
commands plus the boot gate =18**. The ordered commands remain:

1. root/user snapshot selftests + observer synthetic selftests + desktop services;
2. UID1000 UDisks smoke;
3. launch the live-DBus interactive/file-manager probe;
4. original PTY/focused-terminal/readiness barrier;
5. reject stale acknowledgement artifacts;
6. start physical input capture;
7. before screenshot;
8. guarded physical QMP typing;
9. PTY/UID1000/file acknowledgement;
10. after screenshot;
11. raw capture completion/status;
12. original terminal-done acknowledgement;
13. authenticated Dolphin model barrier;
14. Dolphin screenshot;
15. original Dolphin-done acknowledgement;
16. original application clean-exit/model/notification success;
17. `/proc/lockdep` violations gate.

The evaluator accepts Linux **only** with driver rc1, exactly one `17/18 passed`
summary, and exactly the lockdep failure line; any other/additional failure,
wrong total, raw-capture failure, missing exact terminal/backend marker,
observer marker, integrity failure or wrong selftest count rejects it. Guest
acceptance requires rc0, one `18/18 passed`, no failures, successful raw capture,
exact readiness/backend markers, two synthetic snapshot selftest markers,
observers absent and both integrity checks true. No missing evidence is converted
into an accepted sample by the evaluator.

### 3. Two recorded host CPUs; guest SMP4/3072M

The preparer records available CPUs and exactly the first two allowed logical
CPUs, failing if fewer than two exist. Each sample validates the recorded pair
against its current available affinity, applies exactly that pair and verifies
it; no one-CPU or alternate-CPU fallback exists. The profile is in the frozen
manifest. Affinity is inherited by the driver/QEMU processes.

The driver retains `--memory 3072M`, `--boot-timeout 300`, `--timeout 780`.
`qemu-test.py` defaults to SMP4 and passes that value explicitly to make; the
Linux adapter consumes the same SMP and QEMU_MEMORY values. Host memory and
cgroup CPU information are recorded; the plan introduces **no host memory cap**.
Both kernels use snapshot overlays; the shared base disk is not intentionally
mutated by samples.

### 4. Linux first; all three guest steps remain independent

The workflow has exactly one Linux sample and exactly guest1/guest2/guest3,
each once. Each guest step has the same condition:

`!cancelled() && steps.linux.outcome == 'success'`

Because this is an explicit cancellation-status expression, the usual implicit
`success()` condition is not what gates these steps. They do **not** depend on
prior guest outcomes: guest2/guest3 remain eligible after a prior guest failure.
The runner independently checks serialized Linux `accepted` before any guest.
There is no `continue-on-error`; a failed guest step still fails the job even
if later guest steps pass. No extra sample, retry loop or alternate backend is
introduced. Per-sample result creation precedes launch and existing result files
are rejected, preserving failed/interrupted attempts instead of overwriting them.

This guarantee remains subject to explicit cancellation and the inherited
120-minute job/25-minute sample caps. A host/job timeout, cancellation, active-VM
safety rejection or fixture mismatch can leave a sample unexecuted/incomplete;
that must remain **unknown/failed cohort**, never be counted as a pass or retried
to fill in a favorable result. No all-three-completed guarantee can override those
safety/deadline limits.

### 5. Observers off; no fallback/rescue; original diagnostics remain bounded

Workflow defaults leave Qt key and Wayland key observers off, and the acceptance
evaluator rejects their loaded/output markers. Synthetic observer *selftests*
are unchanged prerequisites, not enabling runtime input observers. Physical
keys still require the exact full-line readiness barrier in
`qemu-ready-type.py`; there is no observer fallback, artificial-focus command,
keyboard retry or serial substitute for the physical acceptance evidence.

The pinned runtime's readiness loop still uses the same backend query function,
120-second loop deadline and query bounds. It retains the original query return
code before recording; only that original return code can set terminal_ready.
Failure-only snapshots retain `timeout -k 1 -s TERM 4`; postfailure bulk queries,
pixels and traces cannot rescue the already failed gate. No diagnostic status
is promoted into a successful acceptance result.

### 6. Preparation/freeze and originals

Preparation precedes samples: same-source raw-input consumer copy, signed Linux
6.12.113-r0 install, module/initramfs assembly, candidate ISO build and disk pack.
The manifest then hashes **all four** sample make adapters, host profile, shared
disk, signed Linux kernel/initramfs, 9os ISO/kernel/initramfs/limine config,
selected runtime root helpers and driver/preparer/evaluator/QEMU typing scripts.
The runner requires matching hashes and no active QEMU before and after each
sample. Guest adapters mark ISO and firmware old to prevent rebuilding during
sample launch; the driver contains no mkdisk/repacking step. Linux and guests
use disposable snapshot writes. No active fixture modification is requested by
the plan or permitted as recovery from a mismatch.

There are five `always()` uploads: prerequisite, Linux and each guest. They
include per-sample stdout, serial logs, raw-check output, results/manifests and
screenshots, including failures. Readiness history is printed into the captured
serial log on success **and** failure. Preserve complete originals, not only
selected excerpts; an interrupted job/missing upload remains a recorded evidence
gap. This is not a claim that every entire multi-GB fixture image is uploaded.

## New diagnostic question — appropriately limited

The question is which **first/last existing query observations** precede the
unchanged acceptance outcome: terminal state lines, matching-PID witness,
remote-call errors, completion/unfinished attempts and status-convention labels.
The recorder issues **no additional acceptance query**. It keeps bounded slots
for first/last overall, terminal, remote, incomplete, witness and alarm-status
attempts; unmatched/missing evidence remains `unknown`, not a zero count.
Status142 is an alarm-status convention, not proof of signal cause. Retained raw
prefix/suffix overlap on short output and are not a complete per-attempt log;
selected significant lines survive independently of that clipping.

Recording **and emission occur on success too**. Their I/O and witness reporting
perturb timing; PR42 also changes ABI/service behavior. Thus comparison with
older cold cohorts is not a clean single-variable experiment, original-state
proof, startup cure, causal attribution or statistical reliability measurement.
The document states this explicitly and preserves all earlier failed series,
including the direct-epoll-leaf cohort's whole-plan failure. Even3/3 accepted
here would not erase any hold or complete M40.

Diagnostic history is deliberately **not a new acceptance predicate**. Missing
history/witness or recorder failure can coexist with the original query result;
record it as diagnostic incompleteness, not fabricated zeros, not a changed
application gate, and not evidence answering the diagnostic question fully.

## Runtime pin and manual governance prerequisites

Final runtime paths `kernel`, `userland`, `scripts/qemu-test.py` and
`scripts/qemu-ready-type.py` match exactly
`07fda66bd9a7904e77b7f3e1051df243ee1e4d21`.
Both preparer and workflow explicitly compare those paths to that SHA. The
source diff from runtime to validation head contains only the manual workflow,
plan document and four manual scripts.

Saved local API records confirm:

- fast39168427970: current runtime SHA, completed SUCCESS, all6 relevant jobs
  successful;
- source39168428016: same SHA, completed SUCCESS, all3 build jobs successful
  (stamp success; release skipped is not treated as a source job).

The parent reports review/preservation of actual9 original logs +9 ZIPs on the
never-merge evidence branch at09736fa. I checked local metadata, **not** repeated
that full original-artifact review and did not query GitHub.

The workflow is dispatch-only, contents-read-only, no PR trigger. The document
requires **NO PR**, **NEVER MERGE**, one dispatch after independent review and
zero prior branch runs. Those last conditions are parent/operator governance,
not fully automated by `run_attempt==1`: that field blocks reruns of a run, not
a second fresh dispatch on a fresh runner. Parent must verify zero prior branch
runs immediately before the single dispatch and never merge the replacement
workflow or evidence branch. I did not perform that online check or dispatch.

## Executed read-only/tiny controls and preserved reviewer fixture failure

- `python3 -B scripts/m40-cold-host-tests.py`: **5 tests passed**, exit0.
  `/data/pr42-current/cold-independent-plan-controls.log`.
- `python3 -B scripts/terminal-attempt-history-host-tests.py`: **13 tests passed**,
  exit0; synthetic shell helpers/retention only, no guest or GUI evidence.
  `/data/pr42-current/cold-independent-history-controls.log`.
- `sh -n scripts/m40-cold-driver.sh`: exit0.
- Independent byte comparisons, gate parser, workflow structure, saved prerequisite
  metadata and runtime-path checks: passed.
  `/data/pr42-current/cold-independent-static-controls.py` and `.log`.
- The **first reviewer-only parser fixture failed** because it expected shlex to
  expand literal `$ARCH` to `x86_64`. It did not execute a driver or sample. That
  erroneous first source/log are retained as
  `cold-independent-static-controls-first-fixture.py` and
  `cold-independent-static-controls-first-fixture-failure.log`. The corrected
  fixture checks literal `$ARCH`, the exact shell x86-only guard, and the runner's
  explicit x86_64 argument; no plan assertion or runtime code was weakened.
- No `build/m40-cold` prepared fixture directory existed during final inspection.
  `PYTHONDONTWRITEBYTECODE=1` / `-B` prevented import-cache worktree changes.

Reviewed final plan content SHA256s:

- workflow: `374cd614bea258e460b4f3db8faff09362b77afb51701e21b3a35fa134a95398`
- document: `9757188d1eb7cb33e22792309b366b733336cb723451f221b6b4dda5ff5623ac`
- preparer: `ac1eeb886ee2cc2c5c89f8bcfd2c27324a35b8bad58f17b43765c16ae46701e8`
- host controls: `35d866489920547199382f812aa62a9b8c813fc325f2842cdec00b6f79e289a0`

**All sample outcomes remain unobserved by this review.** Parent exclusively
owns credentials, dispatch, fixture preparation, heavy resources, actual original
artifact preservation, and the one-shot result classification. No retries,
rescue runs, deadline/gate waivers, cold cure or completion claim is authorized
by these host controls or this review.
