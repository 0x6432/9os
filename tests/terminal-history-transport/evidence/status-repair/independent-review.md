# Independent read-only status-fix review — 2026-10-22

## Verdict: no blocker found in the reviewed uncommitted fix

Reviewed current `/data/9os`, branch `work/m40-followup`, HEAD `ed2ce22d514fd703a0d69a5b6858b82fa41a80fa` plus its five modified tracked files and new untracked `scripts/terminal-history-status-host-tests.py`. History transport and UDisks evidence were already cherry-picked. This approval is scoped to transport correctness and host-control coverage, not cold reliability, guest acceptance, or release of any held stack.

### Snapshot identity

- Probe SHA256: `c34251ccea482ebf23283e712ac08a0abbb63d032bb07beb6eb8c33a1170ab1a`
- New status controls: `9e6c8734f11f1cabb4ff5fc8a3a71036cdf55c756dd4e220f27406a2524c5216`
- Modified transport controls: `824c841ec5c5b3cc95cef3b609bece1d7586298af8b66b744b433eec4189334c`
- Modified frozen utility-status controls: `780f42a8bbf86c7bd2b447a59c2294e415b4fcff67ae94bc7df226ba10a443d0`

Probe hash was unchanged after all independent tests. Repository status remained the same six dirty/untracked paths. No source/index edits made by this reviewer.

## Findings

1. **True conditional/OR-list context is covered.** Production line361 remains `emit_terminal_attempts || true`; `set -e` cannot supply reliable internal utility failure handling there. `emit_terminal_attempts()` now checks independent assignment statuses for wc, od and tr rather than relying on the old od|tr pipeline. Complete valid output followed by a nonzero exit is rejected, not mistaken for success. Its stage-specific `KONSOLE_QUERY_TRANSPORT_UNKNOWN` records contain no arbitrary raw payload or FAIL sentinel, return1, and omit the final history terminator.
2. **Framing validates bytes before publishing a slot.** Count must be canonical decimal (0 or nonzero-leading, at most four digits), at most5120; payload must contain only lowercase hex and have exactly twice the declared byte count. Empty input remains valid. Leading-zero/octal ambiguity, malformed numeric metadata, uppercase/nonhex, shortened/odd/empty encoding of nonempty input, and oversized counts fail. `od -v` preserves repeated lines. Binary content is held as hex, not raw shell variables. Count and hex agreement is not a cryptographic content-authenticity claim; utility success and trusted local utility behavior remain assumptions.
3. **Output errors cannot create a valid complete transcript.** Header, slot BEGIN and combined payload/END writes are guarded. Final echo's status is the function status; absent/partial final terminator is rejected by the offline decoder. There is no raw fallback. Additional bounded shell-variable storage is after the readiness loop; no extra on-disk file or acceptance query was introduced.
4. **Acceptance path is byte-identical to pinned PR42 outside this emitter.** Independently loaded `git show 07fda66bd9a7904e77b7f3e1051df243ee1e4d21:<probe>` and compared exact prefix/suffix after removing only the emitter from each version: PASS. Thus query backend, recorder, original eight-second/120-second bounds, query loop, rc use, PTY/state predicates, final raw replay, diagnostics and failure path are unchanged. Missing transport evidence intentionally does not change an existing successful/failed readiness decision; the decoder rejects it as incomplete rather than inventing recovered evidence.
5. **Historical F1 is genuinely frozen.** Modified `terminal-history-utility-status-host-tests.py` executes the checked-in unfixed emitter fixture, not repaired production. It reproduces complete correct od/tr bytes with exit7/exit9, masked emitter status and decoder acceptance. Frozen emitter SHA256 printed independently: `a667c7e8c2affe6c4e3a9752795ce5de6ffecebd9c3acf5b874c4a321a7c74d1`. These passing tests preserve historical failure evidence; they are not demonstrations of a remaining production bug.
6. **Mutation controls remain meaningful.** Raw-fallback mutation now replaces the encoder with frozen raw-emitter behavior, since current validation would reject simply inserting raw text. Lossy/repeat-compression mutants are still rejected. New controls neutralize od and tr assignment status individually while leaving correct output intact; each mutant demonstrably returns0 and decodes correctly, then fails the required UNKNOWN assertion. This targets the semantic status gap, not syntax failure.
7. **Workflow coverage is correctly placed.** Both `.github/workflows/alpine.yml` (job alpine) and `.github/workflows/release.yml` (job build) invoke transport, new status, and frozen utility-status suites exactly once in their existing x86_64 host-regression steps, alongside original history controls and UDisks suites. Both YAML files parsed successfully. This is x86_64 Linux host coverage, NOT execution in aarch64/riscv64 guest shells or proof of Alpine BusyBox compatibility.

## Independent host execution

Ran with `PYTHONDONTWRITEBYTECODE=1`, using existing `/bin/sh` and native host utilities; all exit0:

| Suite | Test methods | Result |
|---|---:|---|
| terminal-attempt-history-host-tests.py | 13 | PASS |
| terminal-history-transport-host-tests.py | 13 | PASS |
| terminal-history-status-host-tests.py | 8 | PASS |
| terminal-history-utility-status-host-tests.py | 2 | PASS, intentionally reproducing frozen F1 |

Status suite covers zero/partial/complete nonzero utility output, empty input, malformed successful output/counts, binary+empty+5120-byte roundtrips, production-loop original pass/fail preservation and query counts, authoritative final raw FAIL sentinel, and independent od/tr semantic status mutations. Transport suite additionally exercises all twelve full slots and strict framing bounds. `/bin/sh -n <probe>` and `git diff --check` both PASS.

## Remaining gates / nonblocking cautions

- New status suite is currently **untracked**: include it with the workflow references when the parent commits the fix. Otherwise those CI invocations will fail. This is an integration checklist item, not a defect in the reviewed code.
- Parent's native and SHA256-pinned Alpine BusyBox utility results were not claimed or duplicated here. `HISTORY_TEST_SHELL` overrides emitter-only tests; production-loop helpers and frozen tests still invoke `/bin/sh`, explicitly documented in the new harness. Do not describe an override-only run as a complete BusyBox execution of every control.
- Native host suites/YAML parsing are not actual Actions execution, guest acceptance, or cold-start proof. New-head CI/review and all prior integration holds remain; no retry, gate weakening, cold-fix or timing-equivalence claim is justified.
- Retained transport excludes the inherited final raw query replay, deliberately unchanged and still capable of triggering the authoritative strict sentinel. Recorder prefix/suffix clipping and timing perturbation remain disclosed limitations.

Only `/data/status-fix-independent-review.md` was written. No builds, VMs, pushes, checkout/index changes, reruns, credentials, or acceptance/deadline changes.
