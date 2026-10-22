# Bounded opaque error identity (diagnostic only)

This addition does not cure cold startup or waive any held integration gate.
The pinned PR42 cohort39215973931 accepted all three guests, but each guest's
first readiness query failed before its second existing within-boot query
passed. Generic `reason=remote-call` discarded the error needed to distinguish
local timeout from a remote DBus error. Old snapshots remain timing-perturbed;
these diagnostics cannot recover an earlier error or prove a kernel cause.

## Wire contract

For a failed synchronous `query()` or asynchronous `node_reply()` with a real
GError, keep the original QUERY end record and original
`DBUS_A11Y_ERROR reason=remote-call` **unchanged**, then append one line:

```
DBUS_A11Y_ERROR_IDENTITY class=CLASS method_hex=HEX method_truncated=BIT bus_hex=HEX bus_truncated=BIT path_hex=HEX path_truncated=BIT remote_name_hex=HEX remote_name_truncated=BIT remote_name_status=STATUS
```

- `CLASS`: `local-timeout`, `remote-dbus`, `other`, or `unknown`.
- `STATUS`: `present` for a recovered remote name, `absent` for a known nonremote
  error, `unknown` if diagnostic APIs/name are unavailable. Unknown is not zero.
- Each HEX field contains lowercase hex encoding of the original string bytes,
  never interpreted as log markup. Source-byte caps: method32, bus256,
  path1024, remote_name256. BIT is1 iff bytes beyond that cap exist, else0.
  Empty HEX is legal, with its meaning determined by STATUS for remote names.
- At most **two identity lines per process**, each **<3584 bytes including LF**;
  total new identity output **<7168 bytes**. No raw message, raw identity,
  arbitrary error code/domain, new acceptance token or synthetic success marker.
- Native Ref bounds are bus<=255 and path<=1023 bytes, so valid accepted
  references correlate fully with no clipping. Constant query method names fit
  the32-byte cap. Explicit flags still describe clipping for synthetic/future
  longer input. Names longer than256 bytes are tested by injection; the real
  IPC fixture uses a249-byte valid DBus error name. No fabricated long name is
  presented as an actual guest observation.

Only new identity records are sentinel-safe. Existing raw QUERY references and
final-query raw replay are intentionally unchanged and are **not** newly
sanitized or exempted from the strict controller. Decode offline; never feed
arbitrary recovered bytes back into the strict transcript as an acceptance fix.

## Public opaque API and meaning

Runtime remains header-free and dynamically loads GIO exactly as before. These
additional symbols are OPTIONAL, looked up from the same existing library:

- `g_dbus_error_is_remote_error(const GError *)` -> gboolean
- `g_dbus_error_get_remote_error(const GError *)` -> allocated gchar*, freed
  with public `g_free(gpointer)`
- `g_error_matches(const GError *, GQuark, gint)` -> gboolean
- `g_io_error_quark(void)` -> GQuark

No GError/object private layout or message field is read. If ANY of the five
symbols is absent, emit class/status unknown and empty remote_name_hex while
retaining method/reference correlation. Do not fail, fallback, retry or add IPC
because an optional diagnostic symbol is unavailable. A remote error whose
public name accessor returns NULL retains class remote-dbus with name unknown.

The installed authoritative GIO2.82.2 public headers were inspected:
`gio/gioenums.h`, `gio/gdbuserror.h`, `gio/gioerror.h`, `glib/gerror.h`.
`G_IO_ERROR_TIMED_OUT` is **24**, with domain `g_io_error_quark()`. The native
model compiles static assertions against these headers for the enum and GQuark
width. G_DBUS_ERROR_NO_REPLY, G_DBUS_ERROR_TIMEOUT and G_DBUS_ERROR_TIMED_OUT
are different DBus error codes; none is substituted for this local GIO enum.

Remote detection has precedence. Only **nonremote**
`g_error_matches(error,g_io_error_quark(),24)` gives class local-timeout.
An explicit `org.freedesktop.DBus.Error.NoReply` reply is remote-dbus, **not**
proof that this process's1500ms timeout fired. Other known nonremote errors are
other. Classification identifies the error API observation, not why the
application/service/runner was delayed or whether any kernel wake was missed.

## Unchanged behavior and lifetime

Original generic errors, return codes, query records/order, exact signatures,
PID authentication, same-node role/focused/showing/visible predicate, tree bounds,
1500ms call bounds, alarm8 and final witness remain authoritative. No new IPC,
query, retry, fallback, deadline extension, cache or observer is introduced.
The existing error still fails closed before the additional diagnostic runs.
`Pending` stores a pointer to its current Ref; `node()` drains BOTH callbacks
before releasing Pending/Ref stack storage, including after the first error.
The additional optional lookups, allocation, formatting and stdout bytes may
perturb scheduling. This is neither unchanged-timing evidence nor a startup cure.
The public name getter allocates a copy, freed immediately after bounded output;
a missing returned name is diagnostic unknown, not a new acceptance failure.

Connection-setup errors, missing-reply/signature errors without GError, and
alarm-killed queries are NOT assigned invented identities. There may be no final
witness after interruption; absent records remain unknown.

## Host-only validation

Native injection (no signed root required):

```
PYTHONDONTWRITEBYTECODE=1 python3 tests/a11y-dbus-host/test-error-identity.py
PYTHONDONTWRITEBYTECODE=1 python3 tests/a11y-dbus-host/test-error-identity-mutations.py --log-dir /tmp/a11y-error-mutants
```

These compile only tiny C models, use real public opaque GError construction and
classification, inject sync/async failures, hide every optional symbol, test
reverse-order callback drains/correlation, byte caps, two-record cap, and reject
independent semantic mutants. They are not IPC/Qt/GUI/cold evidence. The remote
long-name model deliberately replaces only the public accessor to test bounded
transport; it does not claim such a name was received over DBus.

Real private-bus IPC (requires genuine already installed signed x86 root):

```
PYTHONDONTWRITEBYTECODE=1 python3 tests/a11y-dbus-host/test-native.py --signed-root /data/9os/userland/alpine-root-x86_64 --consumer native --log-dir /tmp/a11y-error-native
PYTHONDONTWRITEBYTECODE=1 python3 tests/a11y-dbus-host/test-native.py --signed-root /data/9os/userland/alpine-root-x86_64 --consumer signed-musl --log-dir /tmp/a11y-error-musl
PYTHONDONTWRITEBYTECODE=1 python3 tests/a11y-dbus-host/test-witness-mutations.py --signed-root /data/9os/userland/alpine-root-x86_64 --log-dir /tmp/a11y-witness-mutants
```

Existing30 IPC controls, seven CLI bounds, process interruption and five witness
mutations remain. Four added IPC modes exercise explicit remote NoReply, a FAIL
remote name,249-byte remote name and actual synchronous1500ms GetState timeout;
existing blocked-role/children cases cover asynchronous local timeouts. Ten
additional optional-symbol probes compare unchanged rc and exact final witness
for success and error. The shim is test-only, never installed into runtime.
These run only host x86 processes against an explicit synthetic server, not VMs
or real graphical applications. Completion must be reported separately from
commands merely documented here.
