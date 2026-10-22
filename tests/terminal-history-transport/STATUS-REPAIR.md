# Explicit encoder status repair

The PR42 follow-up combines the existing retained-history hex transport and
bounded UDisks readback evidence without changing either acceptance predicate.
Independent review identified a remaining transport defect: a utility could
emit complete correct bytes and then exit nonzero, yet the unchecked `od | tr`
pipeline and the production `emit_terminal_attempts || true` caller would emit
a normally decodable history section. Correct byte counts cannot reveal this
unreported process error.

## Repair boundary

Only `emit_terminal_attempts()` changes in the Plasma probe. It separately
checks `wc`, `od`, and `tr` statuses before writing a slot, validates canonical
decimal size and exact lowercase hex length, and emits a fixed safe UNKNOWN
record followed by diagnostic failure on any encoding error. A damaged section
has no normal final END and cannot be decoded as complete evidence. Already
emitted earlier slots do not rescue that section.

The original caller remains `emit_terminal_attempts || true`: a diagnostic
failure does not become a readiness veto or a readiness rescue. No extra query,
retry, deadline, PID authentication, focus, same-node state requirement, raw
slice retention, final-query replay, strict controller predicate, kernel, or
manual cold fixture changes. The source-bound hash control verifies that the
rest of the Plasma probe is identical to held PR42.

Intermediate text is bounded by the existing 5120-byte slot contract, kept in
shell variables rather than additional files. Existing 62,464-byte raw storage
and less-than-128-KiB wire bounds remain. Instrumentation still perturbs timing;
none of this establishes a cold-start cure.

## Controls and original observations

- The new status suite was run before the fix: 7 methods, 30 semantic assertion
  failures, no harness errors. Its original log is retained separately from
  successful post-fix output.
- Current controls exercise actual OR-list invocation with zero, partial, and
  complete output followed by nonzero `wc`, `od`, or `tr` status; empty inputs;
  malformed/short successful hex; and invalid/noncanonical/oversized byte counts.
- Independent mutations neutralize only the `od` or `tr` status guard. Both must
  survive shell execution and exact byte decoding, then fail the semantic
  unknown-status assertion. Import, syntax, or setup errors do not count.
- Normal binary/full-slot round trips, no-new-file behavior, original loop
  outcomes/query counts, final raw sentinel rejection, and all prior transport,
  UDisks and readiness controls remain separate obligations.
- The original defect-preserving runner now uses the archived unfixed emitter.
  Its original fixture and all prior failure/provenance artifacts are unchanged.

Native shell mocks and SHA256-pinned Alpine BusyBox tools on the Linux host are
not 9os guest execution, exact-current-desktop-APK validation, cross-architecture
validation, CI success, or observer-off cold reliability evidence. Fresh
current-head guest/CI validation and the existing integration holds remain.
M40 is unfinished; no held PR or evidence/manual branch is merged by this work.