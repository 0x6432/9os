# Retained-history sentinel transport regression

The first checkpoint preserves the unfixed production recorder/emitter/loop at
07fda66 with a synthetic earlier rc1 query containing a source-accepted AT-SPI
object path ending `/FAIL`, followed by ordinary rc0 readiness success. The
shell accepts; the unchanged qemu controller's strict `rb'\bFAIL'` predicate
rejects the historical raw text. The controller expression is source-bound in
the dedicated control; no VM/controller job is executed or parser rule weakened.

`evidence/unfixed-production-loop.stdout` is the exact mocked production-loop
output. `unfixed-production-loop-result.txt` records shellrc0/two queries/strict
rejection. `unfixed-regression.log` records the new two controls and all13 existing
history controls passing with the original collision expectation. These are
synthetic source-contract observations, **not a new actual guest failure**. The
reviewed PR42 ALL3 actual histories did not contain the collision.

The final original-query raw replay is a separate inherited arbitrary-text
caveat; this regression targets only the NEW retained historical payload. A
transport repair must not alter or exempt that replay or relax strict acceptance.
Storage, invocation, PID/authentication, acceptance, deadlines/retries and cold
plan pins remain outside the transport edit. Any recording perturbs timing;
this is neither a startup cure nor original-state proof.

## Checkpoint order and repaired transport

The unfixed checkpoint is `3796b4711a37866f0e07c5bff63049935e9b7167`.
It includes the regression and original failure evidence **before** any probe
change. Its later success witness has focused_states1, qualified1 and
protocol_error0; the earlier legal object path remains in the failed query.

The repair changes **only `emit_terminal_attempts()`** in the production probe.
After the unchanged acceptance loop, `od -An -v -tx1 | tr -d '[:space:]'` emits
each existing slot as lowercase hexadecimal, with a counted BEGIN record,
one hex line and matching END record. `-v` prevents repeated-byte compression.
The whole section ends with `KONSOLE_QUERY_HISTORY_END`. No raw payload fallback
is allowed. Metadata and raw prefix/suffix bytes are encoded together, without
stripping, sanitizing, reinterpreting, or reclassifying any original slot byte.

All twelve original first/last slots and their classifications remain unchanged.
The original 2048-byte prefix plus 2048-byte suffix may deliberately overlap;
this is a reversible transport of the **stored slices**, not recovery of an
unrecorded middle or a complete query trace. Missing witnesses remain unknown,
not zero. Backend/query/PID/authentication, recording, loop deadlines/retries,
query count, acceptance predicates and final original-query replay are byte-for-
byte unchanged. No utility is added to the query-loop path; encoding occurs only
after it has ended. Existing recording and the extra emission still perturb
timing: this is not a startup cure, cold-reliability evidence or original-state
proof.

### Explicit bounds

- Raw retained files remain at most twelve slots × 5120 bytes + 1024-byte metadata
  scratch = **62,464 bytes**; no encoded temporary files are created.
- Hex slot payloads are at most **122,880 emitted bytes**, plus fixed-name framing
  and bounded numeric fields. The hard retained-history section bound is
  **less than 128 KiB** instead of the previous less-than-64-KiB raw section.
- This is only the retained-history wire bound. It does **not** enlarge or bound
  the inherited final raw replay, whole command output, controller timeouts,
  startup deadline, or acceptance budget. The maximum-slot control emits all
  twelve full slots and checks the bound, independent of ordinary fixture sizes.
- Standard `wc`, `od`, and `tr` are used only at emission. Host controls cover
  command failures without changing query success/failure; malformed/incomplete
  transport is rejected by the offline decoder. POSIX pipeline status cannot
  detect an `od` failure hidden by a successful `tr`, but the counted framing
  prevents accepting missing/truncated nonempty bytes as a valid slot. No raw
  fallback or extra acceptance query repairs an emission failure.

### Offline decoding

For one LF-normalized history section (such as an unchanged qemu-test.py log):

```sh
python3 scripts/terminal-history-decode.py history.log --slot first > first.slot.bin
python3 scripts/terminal-history-decode.py history.log --slot terminal-last > terminal-last.slot.bin
```

The stdlib-only decoder never evaluates or executes payloads. It checks header,
section/slot bounds, fixed slot order, duplicate/unknown slots, byte counts,
lowercase hex and matching end records before emitting the chosen slot as exact
binary stdout. Missing slots fail, not empty/zero observations. Surrounding log
bytes are ignored; repeated history sections are ambiguous and must be separated
explicitly rather than guessing which is authoritative. Never decode back into
a controller's acceptance log: reconstructed original text can contain sentinels.

### Controls and remaining caveats

Run `scripts/terminal-attempt-history-host-tests.py` (all 13 original controls,
only the transport-output bound assertion changes) and
`scripts/terminal-history-transport-host-tests.py` (13 dedicated controls).
The latter checks the exact production loop/emitter, strict-source predicate,
all twelve maximum-size binary slots, actual recorder prefix/suffix bytes,
NUL/CR/LF/ESC/non-UTF8, historical FAIL/FAILED/FAILURE and panic/return-code tokens,
original failures, malformed/absent transport, utility failures, exact decoder
CLI output, and raw/lossy/repeat-compression mutations. It hashes the entire
probe outside the emitter against 07fda66, including the final raw replay.

**Inherited final-query caveat:** a FAIL-bearing final raw replay still causes
strict host rejection, even when the shell query mock returns success. A negative
control deliberately preserves that rejection. This fix protects only the new
retained-history payload; it does not claim universal arbitrary-output immunity.

These are tiny native host/mocked-shell controls, not a VM/controller execution,
new guest evidence, target BusyBox validation, or a cold experiment. Current-head
CI and independent integration review remain the parent's responsibility; the
separate cold plan remains pinned to unchanged 07fda66.

Committed host evidence is in `evidence/fixed-host-controls.log` and
`evidence/desktop-pty-host-controls.log`; repaired synthetic output and exact
slot hashes are in `fixed-production-loop.stdout` and
`fixed-production-loop-result.txt`. The all-twelve-full-slot fixture emits
**124,381 bytes**, below 131,072 bytes, with 61,440 slot bytes plus at most
1024 scratch bytes on disk. The `.stdout` artifact is hex-framed; use the decoder
rather than treating its content as readable raw helper text.

## Original first-checkpoint correction

The initially published first checkpoint was c1cf999, not 3796b47. It contained
an inconsistent later rc0 mock witness. The worker incorrectly amended that
already-pushed checkpoint when correcting the fixture. See `PROVENANCE.md` for
exact changes, the coordination error, unchanged outcomes and append-only policy;
`evidence/c1cf999-original/` preserves the original fixture and outputs verbatim.
Neither version is actual guest/cold evidence. No implementation change accompanies
this provenance correction.
