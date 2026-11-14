# Original checkpoint rewrite and fixture correction

## Preserved identities and coordination error

- Original first checkpoint, already pushed by the parent:
  `c1cf999c857f9b32b5a2fdc9def397a53f694529`.
- Amended replacement first checkpoint:
  `3796b4711a37866f0e07c5bff63049935e9b7167`.
- Transport implementation checkpoint, whose implementation is unchanged here:
  `167f2c3cfb2e3814f6804fec11319354e4fca76a`.

The worker used `git commit --amend --no-edit` while correcting the first test
checkpoint, incorrectly assuming its own local commit was still unshared.
That assumption was not verified with the parent. The parent had already pushed
c1cf999 for checkpoint durability; rewriting it therefore broke fast-forward
publication. This was a coordination mistake, not a necessary part of the fix.
The correction should have been a new descendant commit preserving c1cf999.

The parent is preserving the original remote branch unchanged and publishing
167f2c3 on a new `notion/m40-history-safe-transport-v2` remote checkpoint. No
force-push, deletion, rebase or further amendment is authorized or performed by
this documentation change. Going forward, checkpoint commits must be treated as
potentially already published: use append-only commits, never amend/rebase them.

## Exact c1cf999 → 3796b47 changes

Only three files differed (18 insertions, 13 deletions):

1. `scripts/terminal-history-transport-host-tests.py`: one fixture expression in
   `collision_run()` changed. The later rc0 mock previously changed only
   `qualified=0` to `qualified=1` in the common failed-query witness. It still
   said **focused_states=0 and protocol_error=1**, inconsistent with the intended
   ordinary successful helper witness. The corrected fixture additionally sets
   **focused_states=1 and protocol_error=0**. `qualified=1` remains set.
   The terminal line already said focused1, the final summary already said
   models1/OBSERVED, and the mock already returned rc0; those did not change.
   The earlier failed-query fixture, including its legal `/FAIL` path and its
   failed-witness fields, did not change.
2. `evidence/unfixed-production-loop.stdout`: regenerated with the corrected
   fixture while the production probe was still the unfixed 07fda66 version.
   Ten repeated later-witness lines changed those two numeric fields: three in
   each of the last/terminal-last/witness-last records (metadata plus overlapping
   raw prefix/suffix), and one in final raw replay. No other output line changed.
3. `evidence/unfixed-regression.log`: rerun recorded the same successful two
   transport controls and thirteen existing history controls, with new elapsed
   times (0.052 → 0.050 seconds and 2.188 → 2.017 seconds), command labels,
   explicit exit=0 lines, and separating blank lines.

The summary `unfixed-production-loop-result.txt` and initial README were
unchanged. **No production source, strict controller, acceptance predicate,
query invocation, deadlines, recorder/classification, or transport repair was
introduced or changed by this rewrite.** Both checkpoints still precede the
production transport repair.

## What the original error does and does not establish

c1cf999 is a synthetic shell-loop observation with an internally inconsistent
mocked rc0 success witness. It must not be represented as a faithful ordinary
successful terminal-dbus witness, a real helper/guest outcome, or cold evidence.
The shell uses the mocked query's return code, so the original observation still
returned shell rc0 after two queries and exposed the earlier retained `/FAIL`
path to the unchanged strict parser. However, claiming a normal source-consistent
successful follow-up requires the corrected fixture, not retroactive replacement
of the original evidence.

3796b47 separately reran that synthetic observation with a consistent success
witness. Its outcomes remained shell rc0, two queries, earlier FAIL bytes retained,
and strict-source-bound parser rejection. It did not change the acceptance rule
or manufacture an actual guest failure. Reviewed PR42 ALL3 histories had no
observed collision. The separate cohort/cold plan remains pinned to 07fda66.

## Original evidence retained verbatim

`evidence/c1cf999-original/` contains verbatim `git show c1cf999:PATH` copies of:

- the original test script (including the fixture error);
- the original README;
- original unfixed stdout, result summary, and test log;
- the exact complete `git diff --binary c1cf999 3796b47` patch;
- `MANIFEST.txt` with full commit identities, original paths and SHA256 hashes.

These are historical data snapshots, not newly executed controls. The copied
script's relative imports assume its original repository path; do not execute
the archive copy as a current test. The parent's original branch is authoritative
for the complete original checkpoint. This append-only documentation does not
replace either original or corrected evidence or change 167f2c3 implementation.
