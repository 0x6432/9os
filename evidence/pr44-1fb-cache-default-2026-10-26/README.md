# PR44 exact cache-default repair and retained evidence — NEVER MERGE

## Product scope
Exactly the independently reviewed two-path patch on product1fbeb9a18b3529866856a04f327ac2e37eff17c0; patch SHA2562b88b97891468622c8deae65a6acce9e7c15ca31aa37ae8501d1de6d3a3c2793. Helper change is default/comment only: TOOLS_DIR/linux-oracle instead of TOOLS_DIR/dl/alpine-linux-oracle. Explicit override remains unchanged. No sudo, APK flag changes, suppressed errors, signature/inventory/content weakening, workflow changes, or guest deadline changes. Product-local-object.json records exact local product commit/tree/parent. Evidence is append-only on571c56259b39febca8613b6e5e76f7eddebf1d7a; previous entries preserved.

## Failed exact1fb CI originals
Signed run40217589621:5/6 PASS, x86job121241997283 fails before NEW114 native verification/installation/modules/QEMU with PermissionError at default cache mkdir. All6 signed joblogs and6 original artifact ZIPs are retained byte-for-byte with original maps/snapshots/headers. Two originals transferred; ten missing originals reacquired once only from fixed public URLs and matched exact prior size/SHA; new acquisition headers/time are separately recorded, never substituted for original capture headers. ZIP CRCs verified; members are diagnostic json/log/text/PNG only, not package/kernel/root payloads. Source run40217589696 snapshot/originals show source x86 cancelled at ISO upload, RISC/ARM success; cancellation cause remains unproven. No status polling during integration.

## Valid evidence and invalid witness are distinct
Independent A host-only baseline:21 tests/3 semantic assertion failures/0 errors; reviewed candidate21 PASS;13 original semantic mutants killed in BOTH. B valid zero-cap baseline21/3 and candidate21 PASS are separately retained. The earlier before-new-tests files record21/5 under ambient capabilities and are INVALID as a permission witness, not used to establish nonroot EACCES behavior. Do not confuse parent/pre-drop capabilities with post-exec child: valid child UID/GID1000, Eff/Prm/Inh/Amb0, NNP1; broad CapBnd is not effective privilege. Fixture0555dl probe yields genuine EACCES. No new test or repeat assembly was run for integration.

## One retained native assembly — qualified
Only B's previously authorized one offline25 assembly is retained: rc0,25 native signature OKs, exact25 lock inventory pairs,26 input hashes stable, kernel and2 module content/ABI checks. It is NOT an error-free helper run. Raw native.stderr is49 bytes exactly:

```
ERROR: 273 errors updating directory permissions
```

Rootownership/staging metadata remains unreconciled. Verified2.14.12-r0 recipe/source and A's full-archive/9-member byte binding explain the nonfatal directory-reconciliation log counter bypassing NO_CHOWN, not actual273 failed syscalls/errnos/paths or273 distinct directories. Readable consumer file modes/content and retained directory-mode comparisons do not prove no chmod failure. A verified recorded-observation consistency, not the raw installed target filesystem independently.

## Explicit final review boundary
A final narrow-scope PASS: no redesign required for the current staging-only contract. D/apk supplies QEMU kernel plus2 decompressed modules; it is NOT the booted root and is NOT copied wholesale to init. Full root metadata equivalence/security clearance is NOT claimed. Preserve warning, source analysis, capability proofs, raw DB/manifest/content/mode observations. No flag/error suppression is approved. Future use requiring bootable installed root/rootmetadata equivalence requires new assessment. Init cpio ownership is not normalized; PID1 identity is not archive ownership proof.

NEW114 guest90s/RC0 remains UNPROVEN; all guest-boot/M40 holds remain. No rerun/dispatch/manual cancel/merge, no package GET, new native assembly, source execution, VM or CI status polling. PR44 remains open draft/unmerged. API publication is authorized only for these exact validated local objects, evidence first with force:false; stop on mismatch/auth failure/moved heads, never retry writes for metadata lag.
