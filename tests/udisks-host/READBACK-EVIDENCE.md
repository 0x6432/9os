# Bounded typed-readback byte evidence (desktop overlay only)

This closes a specific PR42 archive gap: the actual UID1000 smoke passed all
three typed Gets, but their stdout files were removed without byte evidence.
It adds no DBus call, method, retry, fallback, privilege or acceptance condition.
The original checker source, UID1000 gate, six enumeration checks, system-bus
arguments, 30s/40s enumeration and4s/6s property deadlines remain unchanged.
This is not a cold-start fix, storage-operation feature or M40 completion.

## Records and exact bounds

Before temporary cleanup, each *attempted existing property call* emits at most
one ASCII `udisks_readback_v1` record, with a possible additional safe fallback
if the recorder process errors after partial output. Calls are fixed:

1. vda / Block.Device / `/dev/vda`;
2. vda1 / Block.Device / `/dev/vda1`;
3. vda1 / Filesystem.MountPoints / exactly one root `/` entry.

Metadata names the call ordinal, full object path/interface/property, system
bus, destination, Properties.Get method, original command/checker status, typed
outcome, recording outcome, capture kind, completeness/truncation/unknown flags,
observed captured-byte length and `data_hex`. Identities come only from a fixed
three-entry table; status values are validated0..255 or explicit unknown. Payload
is **lowercase hex**, not raw text or base64: NUL, binary, newline and literal
`FAIL`, `FAILED`, `FAILURE` bytes cannot become the host's `\bFAIL` sentinel.

The reader requests at most1025 bytes and retains at most1024. The extra byte
only detects overflow, never becomes payload. One complete record, including
newline, is checked <=3072 bytes before writing. Shell fallback, including its
leading newline (separates any partial record), is <=512 bytes for fixed args.
There are at most three calls; later calls do not execute after an original
failure. **Added stdout is at most3*(3072+512)=10,752 bytes**, excluding the
unchanged original final success marker and preexisting command/checker stderr.
This is a bytes/read/output bound, not a total wall-clock or filesystem-size
bound. dbus-send's redirected file and the original checker limit are unchanged;
no assertion caps the original DBus stdout file to1024 bytes.

Each call has a fresh property-ordinal file: a failed redirect cannot expose an
earlier call's stale reply. The exact checker runs first only after command rc0.
Recording then reads that file, after status capture, before cleanup. The original
command error wins; otherwise the original checker status wins. Diagnostic
missing-file, helper-exit or output errors cannot veto an existing success,
rescue a failure, trigger another query or overwrite either authoritative status.
No root fallback is introduced. If the output channel itself is unusable, complete
evidence is not guaranteed; the original success marker also needs that channel.

## Do not confuse capture completeness with typed acceptance

- `recording=ok capture=complete complete=1 truncated=0 unknown=0`: nonempty file
  reached EOF within1024 bytes after command rc0. These exact file bytes are
  recoverable. `typed=passed` requires checker rc0; a malformed but complete raw
  file can instead be `typed=failed`, and the original probe still fails.
- `capture=truncated complete=0 truncated=1 unknown=1`: actual1024-byte prefix
  only; never fabricate a full reply or total size. A padded valid reply1025..
  65536 bytes may still pass the **unchanged** checker, while evidence stays
  incomplete. >65536 still fails the original checker.
- Failed command: checker_status and typed outcome are unknown; complete is
  unknown even if the file has EOF. Any captured prefix is real file data, but
  not a complete DBus-response claim. Overflow is flagged if observed.
- Observed empty file: captured_bytes0 and data_hex=empty identify an actual
  zero-byte file, not a fabricated empty DBus result; complete0, unknown1. The
  unchanged checker rejects a successful command's empty reply.
- Missing/unreadable file or recorder failure: recording=error, capture/complete/
  truncation/byte-length/data explicitly unknown. Never substitute zeros or an
  empty string. A preexisting checker rc0 remains successful even if diagnostic
  evidence fails; typed may be passed while capture is unknown.

These bytes are tool stdout, not a separately authenticated system-bus packet
trace or an atomic owner-epoch snapshot. The original fixed typed checks and
all existing general-service/hotplug/usability/authorization limits still apply.
The existing raw stdout/stderr contracts of other probes are not changed.

## Offline recovery from future actual guest artifacts

For a raw desktop/KMS serial log or a timestamp-prefixed GitHub job log:

```sh
python3 tests/udisks-host/decode-readback-evidence.py path/to/original.log \
  --output-dir /tmp/udisks-decoded-new-directory
```

The offline-only decoder validates fixed call identities, method metadata,
statuses, lowercase hex, lengths and completeness. It writes numbered `.reply`
files only for complete command-rc0 captures, with the original exact bytes
(including trailing newlines). A complete but typed-rejected file is recoverable
for debugging and is labeled failed, never promoted to passed. Unknown/truncated/
failed-command evidence produces no reconstructed reply file. Output directories
must be new; files are not overwritten. Multiple command runs remain separate
numbered records, not silently merged. No records => an explicit decoder error,
not three fabricated responses. Unrelated Unicode log lines are ignored.

PR42's original artifacts have no such records and cannot retroactively acquire
reply bytes. Future actual positive signed-guest CI must preserve and decode all
three records in desktop and KMS paths on all architectures. No new guest result
is claimed by the native command mocks below. Product/manual fixtures remain07fd;
parent owns integration with the other follow-up, independent review and new-head
CI only after the current cohort/pinned-fixture work is complete.

## Tiny native command-mock controls

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tests/udisks-host/test-readbacks.py
PYTHONDONTWRITEBYTECODE=1 python3 tests/udisks-host/test-evidence.py
PYTHONDONTWRITEBYTECODE=1 python3 tests/udisks-host/test-evidence-mutations.py
sh -n userland/alpine/desktop-overlay/usr/bin/alpine-udisks-smoke
```

The original eleven test methods and command-order/UID/deadline/rejection
strictness are retained. The positive test now checks the entire exact stdout,
including the three expected records, rather than dropping diagnostic lines.
New controls check exact byte roundtrip/offline recovery, binary/FAIL safety,
1024/1025/65536/65537 boundaries, missing/empty/partial/failed captures, original
command stderr and command/checker status, recorder/output/I/O failures, fixed
metadata, no extra calls and no evidence veto of original acceptance. Eight
temporary mutations require explicit assertion mismatches; syntax/import/runtime
failures are not accepted as successful mutation detection. No service emulator,
VM, kernel/library build, non-x86 execution or new CI job is used here.
