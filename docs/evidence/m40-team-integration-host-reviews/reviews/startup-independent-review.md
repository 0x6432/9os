# Independent read-only review: bounded startup query history

## Scope and verdict

Reviewed `/data/9os-team-startup`, base
`18ef21fc34945f482d262993aa33690070aa86d5` through
`32efc3727248cb3e01c8c5c9bdca2d10a0429633`.
The diff contains only `alpine-plasma-probe` and its dedicated
`terminal-attempt-history-host-tests.py`. No startup-worktree file was modified.

**No blocking acceptance, query-status, retention-bound, or read-only findings.**
One low-severity diagnostic provenance/coverage issue is detailed below. There
is no evidence here of a cold-start fix, original-state reproduction, real GUI
acceptance, or M40 completion. All earlier failures/holds remain applicable.

## Finding R1 — low severity, non-blocking: raw terminal/error classification is not backend-qualified

Evidence: `userland/alpine/desktop-overlay/usr/bin/alpine-plasma-probe:51–54`.
The terminal regex accepts both `A11Y_TERMINAL` and `DBUS_A11Y_TERMINAL` regardless
of the selected backend; `DBUS_A11Y_ERROR reason=remote-call` is also accepted
with the `atspi` backend. In contrast, witness/completion records at lines55–62
correctly check their backend and, where available in the emitted record, PID.

Independent execution of the actual shell functions/loop using the existing
fixture harness produced:

```text
backend=atspi, rc=1, text=DBUS_A11Y_TERMINAL focused=0 showing=1 visible=1
                             + DBUS_A11Y_ERROR reason=remote-call
completion=unknown witness=unknown terminal_line=present remote_line=present

backend=live-dbus, rc=1, text=A11Y_TERMINAL focused=0 showing=1 visible=1
completion=unknown witness=unknown terminal_line=present remote_line=unknown
```

The first case allocates terminal-first/last and remote-first/last; the second
allocates terminal-first/last. Both retain the original failing acceptance result
and make exactly one query. Thus this is **not** a false readiness/qualified-state
finding, nor a claim that such mixed output has occurred in a real guest. The
current names may reasonably mean generic raw-line presence; they should not be
interpreted as authenticated selected-backend observations. Terminal/error lines
themselves have no PID, so the recorder cannot authenticate their PID from that
line alone.

Action: either (a) guard legacy terminal lines with `backend=="atspi"` and DBus
terminal/error lines with `backend=="live-dbus"`, retaining foreign text only in
raw slices; or (b) explicitly document these flags as backend-agnostic raw-presence
flags and retain backend provenance in metadata. Add both cross-backend controls
to `test_exact_lines_pid_backend_and_legacy_completion` (currently lines115–126),
which tests foreign witness/completion but not foreign terminal/error prefixes.
Do not make the diagnostic flags additional acceptance predicates.

## Audited invariants

### Retention and output bounds — pass, with scope clarification

- Exactly 12 possible retained slot names: first/last, terminal-first/last,
  remote-first/last, incomplete-first/last, witness-first/last, alarm-first/last
  (`alpine-plasma-probe:78–88,93`). No attempt-numbered unbounded file sequence.
- Each slot is at most1024 metadata bytes +2048 prefix bytes +2048 suffix bytes
  =5120 bytes. The guarded `cat && head && tail` block does not pipe the actual
  acceptance query or replace its output file/status.
- Slots total at most61,440 bytes (60KiB); the one metadata scratch file adds
  at most1024, giving62,464 bytes (61KiB), below a62KiB retained-history allowance.
  History framing is small and independently bounded by the fixed slot list.
- First slots remain immutable once a regular file exists; last slots overwrite.
  Significant terminal first/last lines are kept separately from clipped raw
  slices; the selected witness line is capped at512 characters before the final
  byte-level metadata cap. Even unexpectedly long numeric fields cannot exceed
  the final metadata/file byte bounds.
- Scope: these are **added retained-history** bounds. They do not cap the original
  `konsole-focus` file, AWK scanning work, or total probe stdout. The original
  last-query `cat` remains at line325 and may emit arbitrarily large helper output.
  The existing `<65000` stdout fixture ends with an empty last query; it must not
  be generalized into a universal total-stdout bound. This is not a new regression.

### Exact records, PID/backend and missing witness — pass except R1

- Witness and completion regexes are anchored end-to-end, use the exact current
  child PID and expected backend, and reject prefixes, suffixes, malformed
  records, and foreign PID/backend records.
- Independent wrong-PID witness+completion case: all four flags remained unknown;
  one query; acceptance failed as expected.
- Independent legacy completion with no witness: `completion=present
  witness=unknown terminal_line=unknown remote_line=unknown`; no synthetic zero
  counts or fabricated witness slots.
- State values match only0/1 in terminal records; witness counts are emitted
  numeric evidence, not inferred absence/zeros or a new readiness condition.
- Status142 is labeled `alarm-status` with an explicit non-causality comment;
  124,137/143 and other nonzero statuses remain distinct. No signal-cause claim
  is warranted by an exit status alone.

### set-e, diagnostic I/O and authoritative query status — pass

- The production helper status is captured immediately in the `if ...; then
  query_rc=0; else query_rc=$?; fi` at line317. Recording never assigns query_rc.
- `record_terminal_attempt || query_recording_failed=1` at318 is explicitly
  non-authoritative. Under the shell's OR-list errexit suppression, the function
  explicitly propagates AWK, metadata pipeline, grouped read/write and redirection
  failures with `|| return 1`; this is important and correctly implemented.
- Readiness still depends only on original PTY publication and query_rc==0 at319.
  Missing text/witness cannot veto original success; claimed focused/visible text
  cannot rescue original nonzero status.
- `emit_terminal_attempts || true` at324 cannot change acceptance. A replay read
  failure can omit history, but does not turn failed readiness into success.
- Independent actual-shell fixture matrix: forced AWK failure, head failure,
  tail failure, and replay-cat failure, each with query statuses0,1,124,142:
  **16/16 passed**. Each made exactly one acceptance query; acceptance remained
  success only for rc0. Recorder failures set recording_failed=1. Replay failure
  is intentionally separate from the recording-failure flag.

### Query count, deadlines, focus — pass

- No added acceptance queries; the single existing query call remains behind the
  same PTY-file checks in the same loop.
- `focused_terminal_query` is unchanged: selected backend only, `timeout -k 2
  -s TERM 8`, no fallback. The120-second readiness deadline, `.1` sleep, child
  liveness check and focus/state acceptance semantics remain unchanged.
- No new focus action or startup sleep was introduced. Recording adds real I/O
  between queries and before final diagnostic replay; its timing perturbation
  is acknowledged in source and emitted history. Unchanged deadline constants
  do not imply identical scheduling or identical number/timing of attempts.

## Executed checks (native x86_64, temporary command/shell mocks only)

```text
PYTHONDONTWRITEBYTECODE=1 python3 scripts/terminal-attempt-history-host-tests.py
............
Ran 12 tests in 2.959s
OK

sh -n userland/alpine/desktop-overlay/usr/bin/alpine-plasma-probe
exit0

Independent diagnostic-I/O/status matrix:16/16 PASS
Independent provenance/missing-witness cases:4 executed (results above)
```

The independent cases loaded the test script with `exec(compile(read_text(),
path, 'exec'), namespace)` and called its `execute` helper, with no bytecode
writes. I/O injections were shell function overrides or in-memory function-text
substitutions in disposable temporary fixture subprocesses; production files
were never edited. The originals and original test run were retained unchanged.
The startup worktree was clean after its test/syntax run and HEAD matched the
reviewed32efc372 commit. No VM, package restoration, heavy build, non-x86 test,
credential access, guest mutation or cold experiment was performed.

Parent still owns CI wiring/integration and real signed-guest validation. The
separate UDisks Python helper remains desktop-overlay-only; this review does not
establish its packaged-printer or real UID1000 guest compatibility.
