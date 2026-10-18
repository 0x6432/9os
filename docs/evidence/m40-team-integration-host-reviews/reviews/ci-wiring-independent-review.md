# Independent CI wiring review — f7b59c1 versus its parent

## Verdict

**No blocking CI-wiring finding identified.** Reviewed pinned commit
`f7b59c121afdeee37e29a192b845d3a9b2306668` against `f7b59c1^`, not a moving worktree diff.
The added consumers are unconditional, separately quoted root/non-root commands
in the intended suites; installer paths and x86-only host guards are consistent.
This is a read-only wiring review, **not a CI/guest pass or merge/hold waiver**.

One low-severity negative-control isolation issue is described below. It does
not remove the positive compiler check that enforces the epoll strict flags.

## Coverage verified

Both workflows retain matrices containing x86_64, aarch64 and riscv64. For each
suite below, both `epollabitest` and `epollrestarttest` occur exactly once as a
root command and once as a separately passed `su user -s /bin/sh -c ...` command:

| Suite/path | Architecture scope | Scheduling scope | Root/user epoll consumers |
|---|---|---|---|
| Source main: release workflow → `scripts/ci-tests.sh` | all3 | default RR | both/both |
| Source MLFQ M25 regressions | all3 | SMP2, explicit `--sched mlfq` | both/both |
| Source Eight-CPU RR poll progress | x86_64 only | SMP8, default RR | both/both |
| Signed main: alpine workflow → `scripts/alpine-tests.sh` | all3 | default RR | both/both |
| Signed Eight-CPU Quick offscreen/Wayland | x86_64 only | SMP8 | both/both |

Exact command tokens were checked with `shlex.split` after YAML parsing or
extraction of the actual multiline shell command. These are `timeout 10 ...`,
not optional `if -x` probes, combined last-status commands, or ignored failures.
The existing five-second restart-consumer process watchdog remains unchanged.
The pinned restart consumer is byte-identical to pre-policy test commit8bef592.

**Coverage boundary:** new epoll consumers are not added to UP smoke or the
separate x86 SMP8 MLFQ scheduler/allocator-stress step. MLFQ coverage is the
explicit SMP2 all3 step; source SMP8 epoll coverage is the RR poll-progress step.
If the intended requirement is both RR and MLFQ at SMP8, the latter remains an
additional coverage request, not something this commit supplies.

## Build/installer and failure propagation

- Source userland uses the unchanged `userland/build-all.sh` → `mkroot.sh` path.
  Its top-level `tests/*.c` glob includes both new standalone consumers for each
  selected architecture and installs them as `/bin/<name>`.
- Signed `scripts/alpine-build-tests.sh` lists `unixpolltest`, `epollabitest`,
  and `epollrestarttest` exactly once each. All three select the same strict
  `-Werror -D_FORTIFY_SOURCE=2` branch, with `-O2 -Wall -Wextra`, the selected
  `$ARCH-linux-musl` target, and exact `/usr/bin/$test` output. Its architecture
  whitelist still accepts all3; its existing `set -eu` preserves build failure.
- Installer mock recognizes all three strict-flag consumers and requires both
  flags. Each new epoll consumer has exact source-count/output validation plus
  independent missing and duplicate list mutations. Earlier installer controls
  remain intact; the shared case-branch edit still matches their anchors.
- `scripts/eventfd-host-tests.sh` invokes the installer model and is itself in
  each workflow's x86-only host-control step, so the added installer checks are
  reachable, not orphaned files.
- All five added host commands (epoll native/model, restart policy, bounded
  readiness history, and UDisks readbacks) are confined to the existing
  `matrix.arch == 'x86_64'` guarded step in both workflows. No non-x86 local
  host-model execution is introduced. Their referenced files exist at the pin;
  the UDisks mock executable mode is preserved.
- Main source controller retains `|| rc=1` aggregation and final `exit $rc`;
  signed main retains `set -eu`. Direct workflow invocations have no
  `continue-on-error`. The unchanged qemu controller requires rc0 and rejects
  FAIL output/panics. No newly added `|| true`, retry, predicate relaxation,
  authentication change, or timeout extension appears in this diff.
- Existing guest log upload globs cover the main, MLFQ and SMP8 logs containing
  the new commands; host-control output remains in the workflow job logs.

## P3 — epoll missing-strict-flag negatives are not target-isolated

**Evidence:** `tests/eventfd-host/install-model.py:128–149` replaces the shared
`flags="-Werror -D_FORTIFY_SOURCE=2"` assignment for each epoll consumer, then
accepts any `CalledProcessError` from the complete installer as detection. The
installer compiles `unixpolltest` before `epollabitest` and `epollrestarttest`,
and the fake compiler enforces these flags for unixpolltest as well. Consequently
those global mutations fail at unixpolltest before reaching the labelled epoll
target. This follows directly from the pinned ordering; I did not repeat the
expensive full installer mock suite to rediscover it.

**Why non-blocking:** the normal `run(script, consumer)` executes the compiler's
strict assertions for each epoll target. Removing a target from the strict case
branch would fail that positive path. Missing/duplicate epoll installation
controls also remain target-specific. The issue is attribution/isolation of the
extra per-target missing-flag negatives, not absence of strict-flag enforcement.

**Suggested follow-up:** mutate flags only for the selected consumer (e.g. an
inserted case-specific assignment), or require/assert that the failing compiler
record is that exact consumer and that preceding consumers compiled successfully.
Keep the two independent flag omissions and earlier Unix controls. Do not label
an earlier Unix compiler rejection as an epoll-target negative observation.

## Independent checks performed

Full output: `/data/team-coordination/ci-wiring-independent-checks.log`.

- Parsed both pinned YAML files; checked all3 matrices, unchanged step counts,
  x86 host guards, SMP8 guards/options, explicit MLFQ options, and exact root/user
  command token counts in five applicable guest paths.
- Inspected signed installer/source-glob reachability, strict-flag cases, and
  exact source/output/count mutation guards; parsed installer model with AST.
- Ran only `sh -n` against pinned contents of the three changed shell scripts.
- Verified restart-consumer identity against8bef592 and inspected controller
  status propagation and existing artifact patterns.

No worktree modifications, installer-suite rerun, consumer/kernel build, VM,
dependency operation, credential access, CI/API operation, push, PR or merge.
The parent's untracked root fixture was not touched. Changed-policy guest/CI
results remain the parent's work. Preserved original-policy root/user72/5
failures remain failures; PR40cold and all cold/integration holds are unchanged.
