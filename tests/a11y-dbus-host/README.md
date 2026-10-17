# WIP: independent AT-SPI bulk-query diagnostic

**Not a replacement for product acceptance.** The independent query has observed
genuine signed Qt/KDE on one Linux control and two 9os x86 boots. It is not
evidence of a kernel or cold-start reliability fix.

The draft uses only public opaque GLib/GIO C APIs and the standard AT-SPI
protocol. It obtains the genuine accessibility bus address, authenticates each
application reference by the bus daemon's Unix PID, retrieves children in one
call per node, and reads the same terminal role and focused/showing/visible bits.
It never trusts a PTY marker, a screenshot, or a label as focus. In Qt 6.8.3,
`QSpiDBusCache::GetItems()` returns an empty array; this diagnostic queries the
live `Accessible.GetChildren`, `GetRole` and `GetState` methods instead.

## Completed native x86 controls

Both the native consumer and the exact signed-musl consumer passed 23/23 real
private-bus protocol cases plus seven CLI bounds. The server is explicitly
synthetic, linked against host GIO, and uses a signed prebuilt x86 DBus daemon.
These are NOT genuine application or guest-kernel observations.

Controls reject wrong PID, missing focus/showing/visibility, wrong role,
short/long state arrays, invalid address, empty/duplicate/oversized application
lists, oversized children, malformed paths, foreign child owners, cycles, menu
decoys, remote errors/timeouts, and independently forged wrong reply signatures
for every queried application method and bus-address reply. The address
signature fixture originally returned a string, which was valid for GetAddress;
strict reason assertions caught this and the fixture now forges a uint reply.

Run after assembling the signed x86 root, with native gio-2.0 development headers:

```sh
python3 tests/a11y-dbus-host/test-native.py --consumer native
python3 tests/a11y-dbus-host/test-native.py --consumer signed-musl
```

Only tiny C consumers are built. No Qt/Mesa/LLVM library compilation. Temporary
binaries, bus configuration and sockets are private test fixtures; optional
`--log-dir` retains bounded diagnostic outputs outside the source tree.

## Genuine fixed-scope observations

One signed-Linux control (17/18, all 17 applicable gates; missing 9os lockdep
retained), then two 9os x86 cold boots (18/18 each), all using immutable fixtures
and disposable snapshots. The new query observed the real Qt keyboard participant
PID and FOCUSED/SHOWING/VISIBLE terminal in each boot, AFTER the original
readiness gate passed. Original UID1000 physical PTY, Dolphin model and fresh
physical display checks were retained. All three had raw390/262 key edges,
outer262 and Qt platform262, zero autorepeats/drops/errors. This batch did not
exercise the failure-only path because original readiness passed in both guests.
Earlier failures remain evidence; timing-perturbed positives do not prove a cure.

Full stopped-VM archives and fixture hashes are retained on the evidence-only
branch `notion/m40-failure-phase-evidence`, commit
`6f1d7e5661222bf16fe5642aff43e32e255391ae`, directory
`docs/evidence/m40-bulk-a11y`. Do not merge binary archives into product code.

## Bounded integration and portable CI

The source is nested at `userland/tests/a11y-dbus/terminal-dbus.c`, outside
`mkroot`'s standalone source-test glob. The Alpine installer explicitly builds
only this tiny executable against signed musl, then copies it to the desktop
fixture. No Qt, GLib, Mesa or LLVM library is compiled by this diagnostic.

The normal terminal readiness gate, its 120-second deadline and eight-second
per-query bound remain unchanged. Only AFTER its irrevocable failure does the
probe run `desktopa11y-dbus` as a separate bounded diagnostic; success cannot
rescue that boot. The default successful path performs no new query.
`ALPINE_A11Y_DBUS_VALIDATE=1` explicitly adds a post-ready query for CI; it must
succeed or the probe exits, so this adds a condition rather than waiving one.
All three architecture PR desktop jobs opt in. Non-x86 builds/tests belong to
PR CI only and remain unverified until the exact current-head jobs succeed.

Run `python3 scripts/a11y-dbus-integration-host-tests.py` for actual shell bounds,
status propagation, unchanged default path, explicit validation failure and
irrevocable failed-gate controls. These shell mocks are not GUI proof.
All existing readiness deadlines, decisions and physical input gates remain.
M40, earlier cold-start/typing failures and integration holds remain open.

## Protocol references

- Qt 6.8.3 `src/gui/accessible/linux/atspiadaptor.cpp`: live method signatures,
  role/state conversion and complete child-reference enumeration.
- Qt 6.8.3 `src/gui/accessible/linux/qspidbuscache.cpp`: empty cache response.
- at-spi2-core 2.56.2 `atspi-accessible.c` and `atspi-misc.c`: current consumer
  semantics and the per-node ChildCount/GetChildAtIndex round trips.

## Explicit readiness-backend candidate (not default; current-head CI pending)

`ALPINE_TERMINAL_QUERY=live-dbus` selects the independent live AT-SPI consumer
BEFORE readiness is attempted. Default/unset/empty remains the existing `atspi`
consumer. Unknown configuration fails before starting the session. There is no
fallback between backends and no retry that reclassifies an earlier failed boot.
Both use the same target PID, genuine terminal role, FOCUSED/SHOWING/VISIBLE
requirements, eight-second per-query bound and 120-second original deadline.

## WIP paired live queries after a real two-host-CPU timeout

Fixed run39050199421, unchanged runtime935bd74, first guest failed8/18 with
valid before/after hashes. It published its real PTY but the live query's
eight-second alarm expired while walking shallow accessible nodes. A preceding
Linux control passed all17 applicable gates under the same actual4-to-2 CPU
affinity reduction. This differs from the earlier missing-PTY failure; it does
not prove the terminal was unfocused or identify a kernel root cause. Original
failure artifacts remain on the separate evidence branch.

This candidate submits a node's independent read-only GetRole and GetChildren
calls together, then drains BOTH completions before releasing callback storage.
Replies may arrive in either order. Authentication, live terminal role/state,
all exact reply signatures, menu pruning, tree/children/application bounds,
1500ms call timeout, alarm8 and the shell120s deadline stay unchanged. No cache,
fallback, focus request, application patch, physical-key injection or acceptance
substitution. GetChildren is also queried for pruned/terminal nodes; any remote
error still fails closed. This is a round-trip scheduling hypothesis, not a
kernel or cold-start cure.

The explicit synthetic paired-valid server withholds each GetRole reply until
the same node's GetChildren arrives, then replies in reverse order. The original
935bd74 serial consumer passes the ordinary positive but fails this new positive
with a real method timeout. The candidate passes both, including exact protocol
ordering assertions. Paired role error, paired children error and blocked
children supplement every old negative; total27 real private IPC controls plus
7 CLI bounds. Native x86 passed; local signed-musl test was BLOCKED before
compilation by missing clang, not a consumer pass. New-head signed-musl, real
Linux/9os, portable PR CI and review remain required. No local Qt/Mesa/LLVM/GLib
library compilation, deadline enlargement or gate waiver. M40 remains open.
Only bulk child enumeration and the public GIO connection path differ.

`python3 scripts/terminal-backend-host-tests.py` executes actual shell dispatch
with synthetic commands: default remains original, live mode forwards the exact
PID/bound, nonzero status is preserved, invalid input runs neither backend, and
all original failure exits/input witnesses remain. This is not VM evidence.
A separate PRIVATE fixed-scope trial completed on 2026-10-17: one signed-Linux
control17/18 (all17 applicable, explicit missing-lockdep failure), then two x86
9os cold boots18/18 EACH, no retries. Both guests observed genuine matching
terminal PIDs926/932 with focused/showing/visible, UID1000 physical PTY command,
Dolphin file-model and fresh physical pixels, clean exit and zero lockdep.
All three raw390/262edges/128taps/0repeat/drop/error; Qt and Wayland observers
were OFF on the application path. Build-free adapter dry-run emitted exactly
one QEMU command, and every stopped-VM fixture hash check passed. The earlier
integrated hash-failed sample remains INVALID, not silently replaced.

Complete archives and the frozen runner are retained on the evidence-only branch
`notion/m40-failure-phase-evidence`, directory
`docs/evidence/m40-bulk-readiness-candidate`. Private probe selection differs
from the checked-in optional shell dispatcher; its actual integrated boot and
current-head all-architecture PR CI remain required. The CI explicitly chooses
`live-dbus` and requires its exact backend witness while preserving the strict
Qt platform observer and every physical/file-model gate.
The candidate is NOT the unchanged original readiness gate. Earlier failures,
source/CI validation requirements and all integration holds remain open.


### Integrated selector failure retained, not a cure

The checked-in selector at bcdf3602dc232ba1f0ae18514e84abbe44a2a731 then
failed its one fixed x86 boot8/18 with VALID before/after fixture hashes:
PTY publication was missing (pty0/terminal0, Konsole PID901). The readiness
query was never called because its genuine PTY prerequisite was not met. The
failure-only independent query found only Plasma PID275, not Konsole901, and
returned applications0/models0. Sender refused all physical typing; empty raw
capture is not input proof. This failure is not silently retried or replaced
by the earlier private18/18 positives. All reliability/integration holds remain.
Complete evidence: evidence-only commit aeeaeb963896d978f3f770f962f1af782253f639,
`docs/evidence/m40-readiness-integrated-failure`.

Current-head PR34 prebuilt jobs at bcdf360: all three actual desktop jobs passed,
but the x86 base host model failed because its extracted shell block omitted
`focused_terminal_query`. Fix the fixture, not the production gate: include the
actual selector and execute ALL eight existing positive/negative/mutation tests
for BOTH backends and all Qt/Wayland trace-flag combinations. The repaired
fixture passes8/8; these remain synthetic controls, not GUI proof. New-head CI
is required; do not substitute the old-head desktop successes.
