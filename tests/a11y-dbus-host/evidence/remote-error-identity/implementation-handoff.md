# Bounded AT-SPI error-identity implementation

## Delivered, uncommitted, isolated

Worktree `/data/m40-remote-errors`, branch `work/m40-remote-error-evidence`, base `e0521e7d5b36540c5b0806d11cc8cae81a476d09`.

Implemented the parent-approved wire contract in `userland/tests/a11y-dbus/terminal-dbus.c` and owned `tests/a11y-dbus-host/*` files only. No index/commit/push/merge/VM/library build or cold cohort. Parent-owned two x86 Alpine lock edits are visible in the worktree and were NOT modified by this agent. Parent retains runtime installation, CI wiring and integration/review ownership.

Final consumer SHA256:
`07c6646292ea1894d82849d04c3f2b662f15e66b892bc0069f0b083e9d7438e8`

## Wire and implementation

After each existing generic `DBUS_A11Y_ERROR reason=remote-call`, append at most one:

```
DBUS_A11Y_ERROR_IDENTITY class=CLASS method_hex=HEX method_truncated=BIT bus_hex=HEX bus_truncated=BIT path_hex=HEX path_truncated=BIT remote_name_hex=HEX remote_name_truncated=BIT remote_name_status=STATUS
```

Classes: local-timeout / remote-dbus / other / unknown. Status: present / absent / unknown. Arbitrary method/reference/remote-name bytes are lowercase hex only; no raw message or identity. Source-byte caps32/256/1024/256; explicit truncation bits. At most2 added lines per process, each<3584 bytes including LF, total<7168. Existing accepted Ref limits255/1023 fit in full. Ordinary constant method names fit32.

`error_identity()` uses optional public opaque GIO/GLib symbols only: `g_error_matches`, `g_io_error_quark`, `g_dbus_error_is_remote_error`, `g_dbus_error_get_remote_error`, `g_free`. Missing any bundle member produces class/status unknown and empty remote-name hex without changing decisions. No GError/object layouts or message fields are read. Returned remote name copy is freed after bounded output.

Remote detection takes precedence. Only nonremote `g_error_matches(error,g_io_error_quark(),24)` is local-timeout. Explicit remote `org.freedesktop.DBus.Error.NoReply` remains remote-dbus, never proof of this consumer's local timeout. Public installed GIO2.82.2 headers verified enum24 and API signatures; native model compiles authoritative header static assertions for G_IO_ERROR_TIMED_OUT and GQuark width.

Sync `query()` and async `node_reply()` preserve generic errors first, then emit identity before freeing the opaque error. `Pending` retains its current `const Ref*`; original `node()` drains BOTH callbacks before releasing Pending or Ref storage. No new IPC, call/retry/fallback, deadline, acceptance or witness changes. Exact QUERY printf source lines,1500ms bounds,alarm8,both-callback-drain loop,final witness and same-node state predicate were independently source-compared to base and remain unchanged. Additional lookups/allocation/output may perturb timing; no timing-equivalence/cold-cure inference.

Focused contract, caveats and commands: `tests/a11y-dbus-host/ERROR-IDENTITY.md`.

## Original failing controls preserved BEFORE consumer change

Files `/data/m40-validation/remote-error-before-fix*` are untouched and all original hashes reverified:

- `remote-error-before-fix.log`: native tiny model compiled; six test methods fail semantically (15 assertion failures) because original consumer emitted no identity, not compiler/infrastructure failure.
- `remote-error-before-fix-consumer.c`: exact base consumer, SHA256 `d3af7800846a3da1fa71d3010771f2f0f1dfc5daa625febadd88f402b3a520b3`.
- Original test/model source snapshots and `remote-error-before-fix-SHA256SUMS` preserve the actual failing controls, not a rewritten reconstructed baseline.

## Actual completed validation

| Validation | Result |
|---|---|
| Native opaque-error injection model | 7 test methods PASS, including each optional symbol independently hidden |
| Independent semantic diagnostic mutants | 10/10 rejected by runtime assertions, not compiler/setup failures |
| Actual private-bus native consumer | 34/34 IPC modes +10 optional-symbol IPC probes +1 process interruption +7 CLI bounds PASS |
| Actual private-bus signed-musl consumer | Same34+10+1+7 PASS |
| Original witness/acceptance mutants | All5 rejected; dependency-copy harness updated without dropping originals |
| Existing shell diagnostic integration | 5/5 PASS |
| Python source parsing, unchanged-source checks, git diff whitespace | PASS |

Both IPC suites used explicit `--signed-root /data/9os/userland/alpine-root-x86_64`, the genuine full root restored by parent (not fabricated/minimal). Both compile only tiny C server/consumers. Native consumer links installed host GIO; signed-musl consumer uses restored signed Alpine loader/libraries. These are x86 host processes and synthetic private IPC, NOT guest/Qt/GUI/cold proof.

Observed in BOTH native and signed-musl logs:
- Explicit remote NoReply: one remote-dbus identity (348-byte line).
- Actual blocked GetRole: BOTH async callbacks drain with two local-timeout identities (287-byte lines).
- Actual blocked synchronous GetState: one local-timeout identity (293-byte line).
- Remote org.test.FAIL: hex-only name (306-byte line), no raw secret message.
-249-byte valid remote name: fully recovered hex without truncation (778-byte line).
- Every optional symbol hidden in success and explicit-error cases: original rc and exact final witness preserved, error identity unknown as applicable.

Native model also tests600-byte synthetic accessor name truncated explicitly at256, long method clipping, full255/1023-byte references, reverse-order callback correlation and hard two-record cap. Long injected names are not described as actual IPC/guest evidence.

Semantic mutants cover: remote→local misclassification, wrong local enum, missing-bundle incorrectly known, wrong bus correlation, uppercase hex, omitted truncation flag, missing sync/async identity, premature callback release, missing record cap. Original witness mutants preserve focus counts, reject invalid state counts/hidden protocol errors/cross-terminal state combination/unauthenticated apps.

## Evidence locations

- `/data/m40-validation/remote-error-ipc-native.log` and `remote-error-ipc-native/` (all original mode/probe outputs).
- `/data/m40-validation/remote-error-ipc-musl.log` and `remote-error-ipc-musl/`.
- `/data/m40-validation/remote-error-after-fix-mutations-expanded.log` and `remote-error-mutations-final/` (baseline+each mutant output).
- `/data/m40-validation/remote-error-original-witness-mutants.log` and same-named directory.
- `/data/m40-validation/remote-error-shell-integration.log`.
- `/data/m40-validation/remote-error-final-source-SHA256SUMS` (all nine owned delivered source/doc files).

## Handoff

New root-free CI commands: `python3 tests/a11y-dbus-host/test-error-identity.py` and `python3 tests/a11y-dbus-host/test-error-identity-mutations.py`. Existing `test-native.py` and `test-witness-mutations.py` CLI interfaces remain; enhanced harness imports the new parser and compiles test-only optional-symbol shim. Existing witness mutation harness now copies those dependencies into its temporary fixture, preserving all five semantic controls.

Owned modifications/new files (all still unstaged/uncommitted): consumer; mock-server.c; test-native.py; test-witness-mutations.py; ERROR-IDENTITY.md; error-identity-model.c; optional-symbol-consumer.c; test-error-identity.py; test-error-identity-mutations.py. Parent should include all dependencies when integrating; no runtime installation of test shims/models.

All previous cold failures, clipping/unknown evidence limitations, original raw-query sentinel caveat, existing strict acceptance and held stack remain. Diagnostics identify API errors, not a service/kernel root cause or reliability rate. Independent reviewer and parent CI/runtime integration remain separate gates.

## Durable small evidence copy

Completed after native/signed-musl/witness tests: `tests/a11y-dbus-host/evidence/remote-error-identity/` contains136 original files /411,453 bytes (~402KiB), plus README and portable SHA256SUMS. All copied bytes matched originals; checksum verification passed. This includes all initial/expanded failures/model/mutant/IPC/witness outputs without pruning or changing raw log bytes. Six incidental parent-owned `remote-error-parent-*.log` checkpoint snapshots were also captured unchanged and are explicitly distinguished from author-run/completed validation; parent owns their final status. Independent reviewer-owned outputs were not copied. No tests switched away from the canonical `/data/9os/userland/alpine-root-x86_64`.
