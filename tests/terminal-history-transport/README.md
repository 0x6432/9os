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
