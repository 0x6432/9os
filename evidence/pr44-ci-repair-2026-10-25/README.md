# NEVER MERGE: PR44 CI repair evidence only

This branch is based directly on4140e542ec1043818e7c5ee0d2c8fbd6dcc511f2, NOT the repaired product head. It preserves original failed/passing CI evidence and independently reviewed native-only repair verification. Never merge this branch into product. Existing PR18–44 cold/integration holds remain; M40 is unfinished.

Product changes are exactly9 reviewed source/test/workflow/lock files in3 separate commits on the existing PR44 branch. No original artifact ZIP, signed APK, full build log or native runtime belongs in product.

## Evidence layout and identity

- `source-originals/`: complete original ARM/RISC-V logs, sourceREST snapshots, original RISC-V test ZIP, access-route provenance and exact synthetic-checkout tree proof. `run-logs-original.zip` is deliberately retained404 HTML access evidence, NOT a real ZIP/log archive. No build ISO is included.
- `signed-and-source-x86-originals/`: six complete signed-run logs, original source-x86 log and four signed-run test ZIPs; all11 payloads independently retrieved and byte-identical to the peer's SHA256/size map. Complete expected map and acquisition results retained.
- Eight REST files suffixed `.new-snapshot.json` are THIS computer's raw newly fetched response bodies. Corresponding `.peer-minified-reconstruction.json` files are locally reconstructed compact equivalents, each exactly matching peer original responseSHA/size. Peer original bodies were direct urllib reads; response formatting varies. Do not claim cross-fetch raw-byte metadata identity. `rest-serialization-provenance.json` records explicit mapping.
- `combined-native-tests/`: local integrated mocked/host-only regression logs. Dash is unavailable on this computer; peer's separately attributed dash8-pass evidence is summarized in reports, not represented as an execution here.
- `native-qsb-and-source-review/`: three signature-verified replacement APKs, dependency identity, before/after native controls, actual signed prebuiltQSB bootstrap and real shader smoke logs. No Qt library compilation or guest proof. `ORIGINAL-WORKSPACE-SHA256SUMS` retains the source workspace manifest (some intentionally unbundled files are listed); root `SHA256SUMS` is the authoritative self-contained branch manifest.
- `reviewed-incoming-patches/`: exact independently reviewed source/fixture/APT/product diffs for audit, not new production files.

## Findings and limits

Original ARM source userland failed with a host-QSB runtime fetch HTTP404. Existing traceback did not print the URL: the exact historical failedAPK is unknown. Three current unavailablepins were independently confirmed and minimally replaced. RISC-V original succeeded using the oldpins and a restored architecture-specific cache; cache masking is possible, not proven perAPK.

APT HTTP/HTTPS30s and retries2 are per-request bounds, NOT an absolute setup deadline or proof of real network recovery. The other original x86 failure was a shell-dependent mutation fixture, not a production status-guard defect. All original failed attempts remain failed; no rerun/cold dispatch was performed.

Actual sourceCI checkout was synthetic3b5116bc00bd87653d41c5bb78f56061fb9e96a5. Its RESTtreeb4e10d1e690b2e22476fe7ce279b213adb574bf5 equals reviewed4140tree, but commitIDs remain distinct. Reports preserve parents/checkout/head identity.

See SOURCE-FAILURE-REVIEW.md and REVERSE-INDEPENDENT-REVIEW.md. Root SHA256SUMS covers every bundled evidence file except itself.
