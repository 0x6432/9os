# Preserved F1: complete correct bytes with nonzero encoder status

Unfixed implementation167f2c3, documentation follow-upd0be01f. The independent
review is `/data/pr42-current/history-transport-independent-review.md`.
Before any production fix, the two source-bound controls here confirmed:

- od emits the complete correct bytes then returns7; emitter rc0, no error marker,
  decoder accepts all original bytes;
- tr emits the complete correct bytes then returns9; same unobserved status.

`unfixed-complete-bytes-nonzero.log`, `unfixed-emitter.sh.txt`, and
`unfixed-utility-status-host-tests.py.txt` preserve the first observation and
exact reproduction sources. Successful control assertions describe the defect;
they are not successful utility outcomes, guest failures, or data corruption.
All original c1cf999/3796b47/167f2c3 and F1 evidence remains preserved.
The upcoming repair is append-only, emitter/tests/docs only, not an acceptance,
controller, final raw replay, kernel, cohort or deadline change.

## First preservation fixture error (retained, not a production failure)

The first new preservation runner called the emitter directly through the
existing emit_slots helper under `sh -e`, rather than production's
`emit_terminal_attempts || true`. That fixture exposed hidden od status but
**failed its tr assertion** (actual direct-call rc9). It must not be counted as
both observations passing. Its original source/log remain above, checkpoint
`a00253f`; production was unchanged.

The next append-only checkpoint corrects only that external fixture to use the
exact real OR-list call context. `unfixed-correct-call-context.log` records both
complete-byte/nonzero cases observed unreported with enclosing rc0 and exact
decode. `unfixed-correct-call-context-tests.py.txt` is that exact corrected
reproducer. No production assertion, deadline, parser or utility status was
relaxed; the control now exercises the real caller context. Production remains
unfixed until after this checkpoint.
