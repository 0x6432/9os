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
