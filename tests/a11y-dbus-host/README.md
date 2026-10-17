# WIP: independent AT-SPI bulk-query diagnostic

**Not integrated into product acceptance and not yet validated against genuine
Qt/KDE in a VM.** It is not evidence of a kernel, graphical, or reliability fix.

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

Next: genuine signed-Linux Qt validation, then failure-only 9os observations.
All existing readiness deadlines, failure decisions and physical input gates
remain unchanged. A later successful diagnostic must NEVER rescue an already
failed readiness gate. Only x86_64 is built/run locally; non-x86 belongs to PR CI.

## Protocol references

- Qt 6.8.3 `src/gui/accessible/linux/atspiadaptor.cpp`: live method signatures,
  role/state conversion and complete child-reference enumeration.
- Qt 6.8.3 `src/gui/accessible/linux/qspidbuscache.cpp`: empty cache response.
- at-spi2-core 2.56.2 `atspi-accessible.c` and `atspi-misc.c`: current consumer
  semantics and the per-node ChildCount/GetChildAtIndex round trips.
