# Independent AT-SPI remote-error review

## Status

Pre-implementation read-only review of `/data/m40-remote-errors` at initial e0521e7. The reviewed `terminal-dbus.c` still reports every call GError as `remote-call`; implementation/proposal has not yet been supplied. No tests executed yet. Native gio headers report 2.82.2. No source, worktree/index, library, VM, or shared-parent changes made; only this report written. Final implementation assessment is pending.

## Baseline invariants to preserve

- Public opaque GLib/GIO ABI; no GError/private object layout access.
- Every query error still sets failed; no classifier outcome may rescue acceptance.
- Existing 1500ms per-call timeout, alarm8, authentication, exact signature checks, tree bounds, same-node terminal state predicate, final witness semantics remain unchanged.
- `node()` submits GetRole and GetChildren, then drains BOTH callbacks before stack Pending storage expires, even when the first completion sets failed. Diagnostics must not introduce a callback early-return or reuse transient Ref/error strings after free.
- Existing reason=remote-call can remain the compatibility error reason while a separate diagnostic records precise identity. Do not use changed diagnostic vocabulary to bypass strict host sentinels or alter query-loop retry logic.

## Adversarial classification checklist

1. Determine explicit remote identity using public `g_dbus_error_is_remote_error` / `g_dbus_error_get_remote_error`; do not infer remote origin from message text, elapsed time, or an error name alone.
2. Local timeout needs exact public error matching (domain plus code), not substring matching of 'timeout', 'NoReply', or translated message text. A remote sender can explicitly send org.freedesktop.DBus.Error.Timeout, TimedOut, or NoReply; those remain explicit remote errors, not proof of local deadline expiry. Remote classification should take precedence over matching timeout codes/names.
3. Unsupported/missing optional identity symbols => identity unavailable/unknown, not fabricated local timeout. Preserve original rc and witness. Partial optional symbol availability needs tests, not only an all-symbols-missing case. No new mandatory LOAD failure or acceptance dependency.
4. Extract/classify/encode before error_free. Public get_remote_error returns newly allocated text: free with matching public g_free, not libc free; never retain a pointer into GError beyond error_free. Optional diagnostics allocation failure must not convert an original failure into success or create arbitrary stderr text.
5. Other local failures (disconnect/cancel/no-server) must not be labeled local timeout simply because no remote identity exists. Missing reply without GError remains missing reply / unknown, not an invented GError.

## Sentinel-safe identity and correlation

- Bound every new identity field and aggregate output; lowercase hex raw remote name/message if retained. Remote names legally contain FAIL and messages permit newlines, return-code tokens, terminal escapes, invalid-looking text. Fixed enumerated metadata must be the only unencoded text.
- Method alone is not enough correlation: GetChildren occurs on registry and multiple nodes; asynchronous completions may reverse. Bind error record to a unique query id plus method/bus/path (safely represented) or equivalent unambiguous tuple from the submit context. Include both sync and async queries and connect-stage scope if supported.
- Store callback identity in Pending with valid lifetime through both drained completions. Ref pointers are safe only if caller keeps the Ref alive until drain; do not point into a variant that was already unrefed.
- Do not assert that the whole preexisting output is universally sentinel-safe: existing QUERY begin raw path can contain /FAIL and inherited final raw replay remains outside the new identity guarantee. Scope safety claims and tests to new diagnostic records, while preserving existing strict rejection behavior.

## Required focused controls

- Real local blocked-role and blocked-children timeouts: exact local timeout classification, no remote name, original rc1 and witness unchanged.
- Explicit remote custom error, plus explicit remote Timeout/NoReply names with timeout-looking messages: remote classification, never local timeout.
- A non-timeout remote name with 'Timeout'/'FAIL'/panic/__RC0 text in its message; lowerhex bounded fields must preserve bytes without sentinel injection. Boundaries at cap, cap+1, empty text, and newline/ESC data.
- Sync GetAddress/GetState error and both async methods; reverse completion order with first callback error and second callback delayed/success, plus both callbacks error. Ensure both ends are emitted before return and each identity matches its own submit context.
- Optional symbols missing individually and collectively: ordinary positive stays positive; remote/timeout negatives remain rc1 with unknown diagnostic identity as appropriate; no mandatory-symbol startup failure. These are diagnostic fallback controls, not permission to weaken required preexisting ABI symbols.
- Preserve all existing positives, 28 negatives, interruption/no-final-witness and seven CLI bounds. Re-run witness semantic mutations. Add classifier mutations (remote-timeout misclassified local; ignored optional-symbol failure; wrong callback correlation) that must fail semantic assertions rather than compilation/setup failures.

## Harness limits

`test-native.py` builds only tiny consumer/server fixtures, but its private daemon requires a signed x86 root even with --consumer native. Parent is restoring that root; do not silently switch daemon/library inputs or build libraries. Native GIO results alone do not validate the signed GIO build, non-x86 ABI, actual Qt application behavior, or any guest/cold-start reliability improvement.

## Pending disposition

Wait for implementation/proposal, then inspect exact diff and execute tiny host controls against the supplied immutable/identified source. Existing merge holds remain unchanged; M40 is not complete.

## Approved contract received (implementation still pending)

Review will use the supplied bounded contract: at most two added identity lines after existing generic remote-call markers; existing query lines/order unchanged; class remote-dbus takes precedence, local-timeout requires nonremote + public G_IO_ERROR/TIMED_OUT(24) match; optional five-symbol bundle missing => unknown with no acceptance veto. Lowercase-hex caps: method32, bus256, path1024, remote-name256 bytes; per-line <3.5KiB, total <7KiB. Pending borrows Ref only until both completions are drained.

Additional contract-specific checks: budget exhaustion must suppress only extra diagnostic lines, never generic errors or callback draining; truncation is byte-based and explicit; absent remote identity must not silently mean an observed empty name when symbol lookup is unavailable. A whole-bundle fallback is acceptable per approved contract (not a blocker if one missing optional symbol disables all five). A real D-Bus error name has protocol length constraints, so over-cap name tests may need a clearly labeled helper/shim fixture rather than invalid wire data; do not count a server-side invalid-name rejection as a passing consumer truncation control.

## Implementation review and independent native controls

Reviewed ready, uncommitted implementation in isolated worktree. Consumer SHA256: `07c6646292ea1894d82849d04c3f2b662f15e66b892bc0069f0b083e9d7438e8`.

**No blocking production-code defect found against the approved contract.** Explicit remote origin is checked before the exact nonremote G_IO_ERROR_TIMED_OUT match. All five optional pointers are checked together; absent bundle remains unknown, without a new exit/dependency. Allocated remote name is encoded and g_free'd before GError free; no message is printed. Per-process two-record cap and fixed byte-capped hex buffers make output statically bounded. Generic error remains immediately before identity; original QUERY lines are unchanged. Async Pending references the still-live Ref; node still drains both completions before returning. No acceptance predicate, timeout, alarm, extra IPC, traversal/authentication condition, or default backend change was found in the consumer diff.

Independent executions (native host, GIO2.82.2):

- `PYTHONDONTWRITEBYTECODE=1 python3 tests/a11y-dbus-host/test-error-identity.py`: **7 test methods pass**. Log `/data/remote-error-model-independent.log`.
- `PYTHONDONTWRITEBYTECODE=1 python3 tests/a11y-dbus-host/test-error-identity-mutations.py`: baseline passes and **10/10 diagnostic mutants rejected** (agent added the callback-drain mutant while review was in progress). Log `/data/remote-error-mutations-independent.log`.

Mutant runner requires unittest assertion failures and rejects compilation/import/setup errors. The early-release mutant intentionally triggers the model's explicit callback-completion invariant abort, which becomes an asserted return-code mismatch; this demonstrates the model detects missed drainage, not an ASan/lifetime proof. The long remote-name overflow control is clearly a public-API helper injection shim (600-byte synthetic name), not an invalid D-Bus wire reply; the IPC long-name case uses a valid249-byte name. These scopes are appropriate and should remain explicit.

### Nonblocking coverage/caveat notes

- Model's `test_real_local_timeout` constructs an actual opaque GError with the correct public domain/code; it is not a real elapsed IPC timeout. Actual blocked-role/children/state private-bus results remain pending signed root readiness.
- Bus/path correlation is direct tuple matching to an actual QUERY begin, not a new unique call-id protocol. This is sufficient for the present no-overlapping-node traversal; do not generalize to future concurrent traversals without strengthening correlation.
- No new globally sentinel-safe transcript claim: existing QUERY raw path remains inherited, and the direct-reference model deliberately has a raw /FAIL path. Only new identity lines are hex-safe.
- Model tests each optional pointer individually for remote and timeout failures. IPC harness additionally compares positive/negative return codes and final witness under each missing optional symbol. These still need independent IPC execution; no current pass claimed.
- Witness-mutant harness now copies the imported identity parser and optional-symbol shim into isolated mutant trees. Those are the dependencies used by test-native.py; model.c is not needed for a parser-only import. Independent execution of the five IPC witness mutants is still pending root readiness.
- Diagnostic output write failure is not a new acceptance veto, consistent with diagnostic-only contract; do not interpret absent identity lines as proof of no GError. Budget exhaustion after two identities also does not erase generic failures.

### Remaining validation gate

Waiting for parent's canonical complete signed-root readiness notification before running real private-bus native controls and witness mutations. Root/package lock updates are parent-owned and were not reviewed or altered here. No VM/library build, source edits, or git/index actions performed.

## Independent actual private-bus validation completed

Ran (no source edits; canonical root frozen/read-only):

`PYTHONDONTWRITEBYTECODE=1 python3 tests/a11y-dbus-host/test-native.py --consumer native --signed-root /data/9os/userland/alpine-root-x86_64 --log-dir /data/m40-validation/remote-error-independent-ipc`

**PASS, exit0: 34/34 real private IPC cases + 10 optional-symbol IPC probes + 1 explicit SIGALRM interruption + 7 CLI bounds.** Main log: `/data/m40-validation/remote-error-independent-ipc.log`; per-case transcripts: `/data/m40-validation/remote-error-independent-ipc/`. Consumer source hash before/after remains `07c6646292ea1894d82849d04c3f2b662f15e66b892bc0069f0b083e9d7438e8`.

Confirmed actual elapsed blocked GetRole/GetChildren/GetState errors classify local-timeout; explicit remote org.freedesktop.DBus.Error.NoReply remains remote-dbus; sentinel and valid long remote names roundtrip safely; individually hidden optional symbols preserve rc and final witness. Existing positives/negatives, caller ordering, tuple correlation, and no-final-witness interruption checks pass. This is synthetic private IPC with host GIO and signed daemon, not signed-musl consumer validation, actual Qt/GUI, guest-kernel, non-x86, or cold-start proof. Implementation agent owns its separate signed-musl/witness runs; those are not counted as this reviewer's executions.

## Two-lock dependency update review

Compared JSON structurally against worktree HEAD: exactly one package entry changes in each of `userland/alpine/locks/x86_64.json` (index80) and `x86_64-desktop.json` (index437). Only URL and SHA256 change:

- same v3.22/main/x86_64 repository, libexpat2.8.5-r0 ->2.9.0-r0;
- SHA256 becomes `4c9fb621d455d12475767e261821a4be7b4e91e2ba19c20a02e7fa99189b0d6d` in both.

No package count/order, other dependency, minirootfs hash, cache setting, architecture, profile, or top-level key changes. Independently hashed the cached replacement APK at `/data/m40-tools/dl/alpine/x86_64/libexpat-2.9.0-r0.apk`: exact new lock digest. Canonical root `/etc/9os-alpine.json` contains the same new URL/digest and x86_64 arch. Retained `.PKGINFO` in `/data/m40-validation/libexpat-new-pkginfo.txt` reports sole dependency `so:libc.musl-x86_64.so.1` and compatible `so:libexpat.so.1=1.13.0`; installed files retain libexpat.so.1 soname.

Inspected retained provenance: `libexpat-direct-signature.log` reports package signature OK; `libexpat-signed-index.log` records the stable repository index and new package; `signed-base-updated-lock.log` ends with successful full160-signed-APK restore. `libexpat-fetch-verify.log` preserves an earlier unable-to-select-package failure; it is NOT the successful signature evidence and must not be silently counted as such. I did not independently re-run crypto verification or rebuild/modify the root. That verification remains grounded in parent's retained direct-signature logs plus this reviewer's digest/manifest check.

**Disposition:** no blocking issue found in diagnostic implementation or minimal two-lock diff. Host native IPC and model/mutation review gates above pass. Preserve parent-owned current-head workflow, signed-musl, portable CI and underlying reliability holds; no M40 completion or guest acceptance claim.
