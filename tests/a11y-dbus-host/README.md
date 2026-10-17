# WIP: independent AT-SPI bulk-query diagnostic

This checkpoint is **not integrated, built, or validated yet**. It is not a
replacement for the existing terminal-focus acceptance gate and is not evidence
of a Qt, kernel, graphical, or reliability fix.

The draft uses the public opaque GLib/GIO C ABI and standard AT-SPI protocol to
query the real accessibility bus, authenticate each application reference by its
bus connection's Unix PID, retrieve children in one call per node, and read the
same terminal role and focused/showing/visible state bits. It never trusts Qt's
empty cache `GetItems`, a PTY marker, a window screenshot, or a label as focus.

The synthetic server is explicitly a native private-bus protocol test fixture;
it must never be presented as genuine GUI evidence. The test runner is currently
a private sandbox prototype and still needs portable build/temporary-output
handling and negative-control validation before any PR is opened.

Next: native x86 private-IPC controls; signed-musl tiny-consumer build; failure-only
Linux/9os comparisons. All existing readiness deadlines, failure decisions and
physical typing gates must remain unchanged. Do not compile Qt/Mesa/LLVM locally
or build/run non-x86 targets locally.
