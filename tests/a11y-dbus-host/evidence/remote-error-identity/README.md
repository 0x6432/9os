# Complete small error-identity validation evidence

Preserved unchanged from `/data/m40-validation` after all author tests completed.
136 original files, 411,453 source bytes (401.8 KiB), before this README/checksum manifest. Every copied byte was compared with its original. `SHA256SUMS` uses portable paths within this directory; the original before-fix and final-source manifests remain unmodified, including their original paths. No log pruning, normalization, or rewriting.

Includes the original failing native controls and source snapshots, both initial and expanded model/mutation results, all per-mutant outputs (initial9 and expanded10), complete native and signed-musl private-IPC mode/optional-symbol/interruption outputs, all five original witness-mutant outputs, and shell integration log. Intermediate results remain present, not superseded or removed.

Completed native and signed-musl suites each report34/34 IPC modes +10 optional-symbol probes +1 process interruption +7 CLI controls. Model7 methods pass; expanded diagnostic mutants10/10 and original witness mutants5/5 are rejected. Before-fix failures remain failures. These compile/run only tiny x86 host C consumers and explicit synthetic servers, not guest/Qt/GUI/cold evidence.

Both actual IPC suites used the explicit immutable canonical root `/data/9os/userland/alpine-root-x86_64`. No alternate/minimal root was substituted. Parent owns signed full160-lock restoration and the narrowly scoped libexpat update, runtime installation, CI wiring and integration. Independent-review outputs are intentionally not copied here because they are reviewer-owned; parent may package those separately.

Verify copies from this directory with `sha256sum -c SHA256SUMS`. Original files under `/data/m40-validation/remote-error-before-fix*` were independently reverified and left untouched. Final implementation handoff remains `/data/remote-error-implementation.md`. The source snapshot identifiers in logs describe their actual run checkpoints, not an assertion that every intermediate model used the final consumer. No acceptance/deadline weakening, new cold cohort, commit/index/push/merge/VM/library build was performed by the author.

## Concurrent parent log snapshots

The small prefix-matched copy additionally captured six `remote-error-parent-*.log` files already present during packaging. They are preserved byte-for-byte as checkpoint snapshots, not described as independently run author controls or proof that all parent checks had completed. Parent owns their final status and may archive later authoritative outputs separately. Their presence does not replace or alter any original author/model/mutation/IPC evidence.
