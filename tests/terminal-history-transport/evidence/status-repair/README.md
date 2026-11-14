# Fresh host validation — explicit utility status repair

These are host/mocked-shell controls, not guest or cold-start evidence.
The byte-preserved before-fix.log.gz records 7 methods and 30 semantic assertion
failures; gzip preserves the original unittest trailing spaces. Uncompressed
SHA256: d239305b8cd9d813f9eb5d83773f72a48c0d325f78a0cea20113186eb185cab6.
The final selected 15 suites pass: 125 unittest methods plus nine UDisks
mutation controls. Historical frozen-F1 methods intentionally reproduce the
old defect and are NOT current-production fix proof.

BusyBox emitter controls use the SHA256-pinned Alpine 3.22.6 x86_64 minirootfs
27694aaa55fd7a9e3ef596e0ad4eb66802308bb20172b17030cd5f4d8ae9bac2
(BusyBox 1.37.0), executed via its musl loader on the Linux host. The emitter
uses BusyBox sh/wc/od/tr; the transport suite uses native /bin/sh with the
BusyBox utility wrappers. Production-loop helper invocations remain mocked.
This is not the current exact desktop APK or a 9os kernel/guest test.

Both workflow files parse as YAML and contain each added diagnostic suite
exactly once. Independent review found no blockers and independently checked
all Plasma-probe bytes outside the emitter against held PR42.

No guest/architecture CI was run locally, no cold experiment was dispatched,
no held PR was merged, and no original acceptance predicate/deadline changed.
The diagnostic-only history and UDisks changes are integrated from existing
isolated source commits; the new encoder-status fix and CI controls are added
on a separate working branch. M40 remains unfinished.

Host: Linux-6.18.49-x86_64-with-glibc2.34
