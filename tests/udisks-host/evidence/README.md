# Original evidence gap and control checkpoint

Branch starts at product07fda66. `original-gap-native-baseline.log` and
`original-eleven-controls.log` are **new native command-mock baseline controls**,
not rerun guests and not relabeled historical PR42 evidence. The historical
`baseline-gap-control.py` intentionally applies only before evidence is added;
it is not part of the future acceptance-test command list.

The preserved PR42 independent review is copied verbatim from the existing
`/data/pr42-current/services-runtime-independent-review.md`. It distinguishes the
six actual UID1000 positive guest executions from the absent archived individual
Property.Get replies. The new feature addresses that precise archive gap only.

Original independent raw-sentinel/optional ARM-lock assumption-error source and
stdout/stderr were inline tool-call material, not separate files found alongside
the preserved review after the sandbox reset. **Standalone original source/log
files are unavailable here.** The existing review preserves their summaries and
limitations, including the missing checked-in ARM lock assumption. No original
experiment was reexecuted/reconstructed and claimed as historical evidence.
No actual readback-byte archive is retroactively fabricated for PR42.

The attempted coordination POLICY.txt path remains absent after reset. Current
parent restrictions apply: isolated worktree only, no auth/push/CI/VM/heavy or
non-x86 execution; parent owns integration and product/manual fixture07fd stays
untouched. Only dedicated UDisks overlay/test/docs changes are in scope.
