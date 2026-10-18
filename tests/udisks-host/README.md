# Scoped read-only UDisks acceptance controls

Run on a native x86 host, with Python 3 and the normal shell/core utilities:

```sh
python3 tests/udisks-host/test-readbacks.py
sh -n userland/alpine/desktop-overlay/usr/bin/alpine-udisks-smoke
```

These are command mocks, **not a fake replacement DBus/storage service** and not
real-guest evidence. The test executes the actual overlay consumer, redirecting
only its installed Python helper path into the worktree. Mock `id`, `timeout`
and `dbus-send` do not contact any bus or storage device. The complete ordered
command arguments and first-failure stop behavior are asserted; only the original
ObjectManager method and three public `Properties.Get` calls are allowed.
No root fallback, retry, mutation method or privilege policy is introduced.

## Concrete gap and fixed scope

Previously six independent `grep` checks could be satisfied by unrelated objects
or properties in `GetManagedObjects`. They remain mandatory and unchanged, with
the same system-bus destination and 30-second reply / 40-second kill deadline.
The consumer now requires UID1000 and also reads:

- `/org/freedesktop/UDisks2/block_devices/vda`, `org.freedesktop.UDisks2.Block.Device`
  = the NUL-terminated bytestring `/dev/vda`;
- `/org/freedesktop/UDisks2/block_devices/vda1`, `org.freedesktop.UDisks2.Block.Device`
  = the NUL-terminated bytestring `/dev/vda1`;
- that same vda1 object's `org.freedesktop.UDisks2.Filesystem.MountPoints`
  = an array containing exactly the NUL-terminated bytestring `/`.

Each extra readback has its own 4-second reply / 6-second kill bound, no retries;
this adds at most three such bounded operations, not a longer enumeration or
application-readiness deadline. Failure preserves the command's nonzero status
and stderr and cannot emit the existing acceptance marker. The new Python helper
is deployed by the existing desktop-overlay copy; no build-system/CI change or
new library/package restoration is required (this overlay already uses Python).

`dbus-send`'s printer emits strings without escaping them. Therefore this helper
is intentionally **not** a general parser for arbitrary ObjectManager replies.
It matches the complete known typed readback, including the variant, ay/aay
shape, identity, NUL suffix, and absence of additional arguments. Printable fixed
identities are emitted as quoted byte arrays, not hex. Unknown output shapes,
non-ASCII, malformed/truncated/empty/oversized replies and extra mount points
fail closed. The 64KiB helper read cap is not a claim that the upstream
`dbus-send` output file itself is byte-capped.

The printer's variant/array/bytestring behavior was inspected in upstream
`tools/dbus-print-message.c` (`print_iter` and `print_ay`), alongside repository
DBus consumers and the existing smoke gates. The initially attempted GitHub
`dbus-1.16.2` source tag was unavailable (HTTP404); the published upstream master
file was inspected instead. Actual signed guest printer compatibility remains a
CI requirement, not a result established by these handcrafted mock fixtures.

## Dedicated rejection cases

Eleven test methods cover the exact positive UID1000 command sequence; alternate
printer whitespace/line endings; all six original missing gates; UID0/999/1001
before any bus call; and absent service, access denied, disconnected,
unknown object/interface/property, timeout and killed-command failures at each
of four request stages (including plausible stdout on a failed call).
Successful but empty replies/empty object maps, disk/partition identity mismatch,
wrong root, empty MountPoints, wrong types, missing NUL, extra arguments,
truncation, unsupported identity/property contracts and missing files fail.
An explicit decoy-object fixture passes every old grep but fails the scoped
readback. Extra mount points and newline-based text-record injection fail too.

## Precise limitations and required real-guest CI

This remains the existing **fixed virtio disk-root smoke**, not a general device
browser, complete ObjectManager parser, size/UUID validator, or proof of Manager
interface presence. It deliberately rejects additional vda1 mount points. The
three properties are separate reads, not an atomic snapshot, owner-epoch
validation, or a live hotplug monitor; device/service changes between reads may
fail. It validates neither mount authorization nor any storage write operation.

Parent must wire/run these host controls in CI and deploy the complete overlay
for real UID1000 `alpine-udisks-smoke` on x86_64/aarch64/riscv64 guest CI. Verify
the actual packaged UDisks reply types and root mount list, original strict
service gates and all unrelated desktop acceptance gates. No local VM,
non-x86 execution, Qt/Mesa/LLVM/GLib build, fixture mutation or cold experiment
was run for this change. No cold-start fix, reliable desktop, or M40 completion
is claimed; the PR18→41 holds and original PR40 cold failure remain unchanged.
