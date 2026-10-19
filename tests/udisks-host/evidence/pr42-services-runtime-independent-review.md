# PR42 independent signed-runtime services and history-sentinel review

## Outcome and scope

**Confirmed six real preserved guest UDisks successes: UID1000 in both desktop
and KMS paths on aarch64, x86_64 and riscv64.** Source-to-runtime control flow
requires all three scoped typed readbacks before the success marker. This closes
the earlier *unvalidated packaged-printer / actual guest UID1000 positive-path*
gap for these exact signed-runtime configurations; it is not a general service,
privileged storage, usability, or cold-reliability completion result.

**No observed history/FAIL-sentinel collision in the three actual KMS histories
or the helpers' fixed status vocabulary.** A broader arbitrary-raw-text immunity
claim is not justified; the source-accepted object-path caveat below is explicit.
No acceptance parser was weakened or changed.

Reviewed current integration checkout
`07fda66bd9a7904e77b7f3e1051df243ee1e4d21`, three original timestamped job logs,
and the original desktop evidence ZIP members, not just filtered logs or green
job conclusions. Read source and ran tiny local mocked-shell/static checks only.
No new guest/non-x86 execution, VM, build, authentication, CI, push, package
restoration, or worktree modification occurred.

## 1. Provenance and preserved originals

`39168427970.json` records alpine-prebuilt-runtime completed/success, head07fda66,
with all six fast jobs successful. `39168428016.json` records build-and-release
completed/success at the same head, with all three build jobs successful. The
service conclusions here come from the three desktop jobs, not an inference
that every full-source job separately exercised these signed desktop services.
All three desktop job logs show checkout of merge3d82350, explicitly merging
07fda66 into18ef21f (line123); each architecture is identified in its job env.

| Architecture | Original job | Desktop evidence ZIP | ZIP SHA256 |
| --- | --- | --- | --- |
| aarch64 | job-117796202945.log | 12151762243.zip | 5dc9f4a47cd935e11431a91f3c405685afcf6be4748768d909f9b891ac77e06c |
| x86_64 | job-117796202967.log | 12151532724.zip | 3fc41076ffd278fba538cb03052c1bac542961b6c4b6feaab49f3485b7ed9e5b |
| riscv64 | job-117796202979.log | 12151722599.zip | fe8f855584a74b41e4029f9641bd4ec0f811efc4064e8aa17986070e41666c28 |

Each ZIP contains separate `build/test-alpine-desktop-ARCH.log` and
`build/test-alpine-kms-ARCH.log`, plus `userland/build/alpine-desktop-ARCH.lock.json`.
All three generated locks identify profile desktop and the correct architecture,
with SHA256 for every package:593 entries on ARM/x86 and579 on RISC-V. The x86
package map exactly matches the checked-in x86_64-desktop lock. ARM/RISC-V do not
have checked-in architecture-specific locks here; workflow alpine.yml:182–183
resolves and then rebuilds offline using the generated lock. Do not claim those
maps were compared against nonexistent checked-in cross-architecture locks.

All three preserved locks include UDisks2 2.10.1-r3, DBus1.16.2-r1 and
Python3.12.15-r0. Original job output includes package verification `0 - OK` and
installation records. `userland/alpine/build.py:157,185` uses target signing keys
and mandatory `apk verify`; the workflow repeats installation from its generated
lock offline. This is the preserved signed/pinned-runtime pipeline evidence,
not a fresh independent signature verification of APK bytes in this review.
The ZIPs do not contain APKs or a complete guest root image.

## 2. Six actual command / UID / return-code triplets

Line references below are original job-log line numbers and raw ZIP-member line
numbers, with CR removed for counting. Every triplet is the actual command,
`alpine-udisks: real disk/partition/root filesystem enumerated uid=1000`, then
zero return code. Each original job also has two final PASS summary entries for
`su user -s /bin/sh -c alpine-udisks-smoke`.

| Architecture / path | Job log lines | Raw ZIP log member / lines | Result |
| --- | --- | --- | --- |
| ARM desktop | 6364–6366 | test-alpine-desktop-aarch64.log:3084–3086 | uid1000, __RC26=0 |
| ARM KMS | 12070–12072 | test-alpine-kms-aarch64.log:106–108 | uid1000, __RC1=0 |
| x86 desktop | 6461–6463 | test-alpine-desktop-x86_64.log:3059–3061 | uid1000, __RC26=0 |
| x86 KMS | 11774–11776 | test-alpine-kms-x86_64.log:92–94 | uid1000, __RC1=0 |
| RISC desktop | 7676–7678 | test-alpine-desktop-riscv64.log:4449–4451 | uid1000, __RC26=0 |
| RISC KMS | 14763–14765 | test-alpine-kms-riscv64.log:1487–1489 | uid1000, __RC1=0 |

Both scripts invoke the same installed desktop-overlay consumer as `su user`:
`scripts/alpine-desktop-tests.sh:28` and `scripts/alpine-kms-tests.sh:43`.
The consumer explicitly rejects any UID other than1000 before contacting DBus
(`alpine-udisks-smoke:5–8`); this is not a root-only service query mislabeled as
unprivileged. Root service/eudev startup remains a separate prerequisite.

### What those successes establish about mandatory typed reads

`userland/alpine/desktop-overlay/usr/bin/alpine-udisks-smoke`:

1. Lines11–20 still require public system-bus ObjectManager.GetManagedObjects and
   every original Manager/disk/partition/Filesystem/device/root-fragment gate.
2. Lines26–32 issue public system-bus Properties.Get to org.freedesktop.UDisks2,
   followed by the installed Python checker, with no retry/root fallback.
3. Lines34–39 invoke all three checks in order:
   - vda object, org.freedesktop.UDisks2.Block.Device = `/dev/vda`;
   - vda1 object, org.freedesktop.UDisks2.Block.Device = `/dev/vda1`;
   - that same vda1 object, org.freedesktop.UDisks2.Filesystem.MountPoints = `["/"]`.
4. The success marker is only printed afterward at line40. Top-level calls run
   under set-e and are not inside an ignored/conditional failure context.

`udisks-property-check.py:17–29,32–44` requires a method-return header and an exact
whole-body variant containing NUL-terminated byte arrays: ay for Device, aay for
MountPoints, including `+ \\0` printed suffixes, with no additional arguments.
Malformed, wrong-identity, absent, non-ASCII or oversized helper input fails.
Thus the six actual success markers plus rc0 support successful mandatory checks
against the actual packaged printer/Python, not just the eleven command mocks.

**Evidence limitation:** the three Get replies are redirected into a temporary
file and removed on exit, not echoed into these artifacts. There is no packet
trace or independent per-property raw-reply archive here. Confirmation is from
the mandatory same-source execution chain and its actual successful completion;
do not describe it as direct inspection of preserved per-property reply bytes.
Builder build.py:199–200 deploys this helper only for profile desktop. Its Python
dependency remains desktop-only, not assumed present in the base fast root.

## 3. History raw text versus the exact host FAIL sentinel

Current `scripts/qemu-test.py:122–123` passes a guest command only with a zero
marker, no kernel panic, and **no `re.search(rb'\bFAIL', body)` match**. This is a
prefix match: FAILURE/FAILED can also match; it is not only an exact word FAIL.
Emitting history inside an ignored shell diagnostic function does not exempt
that text from the outer host parser. KMS eventually cats the entire plasma log
in its completion command (`alpine-kms-tests.sh`, final status/model check).

### Actual preserved histories: no collision

All three KMS ZIP logs contain `attempts=2 recording_failed=0 timing_perturbed=1`:

| Architecture | History header line / PID | First attempt | Last attempt |
| --- | --- | --- | --- |
| aarch64 | 781 /551 | rc1, models0, visited116, NOT_OBSERVED | rc0, models1, visited7, OBSERVED |
| x86_64 | 784 /612 | rc1, models0, visited116, NOT_OBSERVED | rc0, models1, visited7, OBSERVED |
| riscv64 | 2162 /629 | rc1, models0, visited116, NOT_OBSERVED | rc0, models1, visited7, OBSERVED |

Each first witness reports terminals1, valid_states1, focused_states0,
showing_states1, visible_states1, qualified0, protocol_error0. Each final witness
reports focused_states1/qualified1 with the other observed counts unchanged.
These are explicit preserved observations, not inferred missing-witness zeros.
Each architecture emits six history slots (first/last, terminal-first/last,
witness-first/last). **All18 emitted slot bodies were checked against the exact
byte sentinel: zero matches.** Observed application paths are root or numeric Qt
AT-SPI paths; none contains FAIL. Desktop logs do not have interactive history.

The fixed helper contracts do not print FAIL when a readiness query returns1:
- desktopa11y.c terminal output ends in PASSED or NOT_READY;
- terminal-dbus.c:46,172–174 uses DBUS_A11Y_ERROR reason=remote-call etc., numeric
  witness fields, and OBSERVED or NOT_OBSERVED;
- history metadata records numeric rc/status/backend/PID without inventing FAIL.

Tiny mocked-shell executions of the actual current recorder/loop confirmed:
1. live-dbus NOT_OBSERVED then OBSERVED: shell rc0, two queries, no sentinel hit;
2. live-dbus remote-call then OBSERVED: same;
3. atspi NOT_READY then PASSED: same.
These are contract controls, not new guest observations. No currently observed
failed-query status token inevitably converts a later successful command to a
host failure merely because it is retained in history.

### Remaining raw-text caveat — source-level possibility, not a PR42 guest failure

Do **not** generalize the above to all possible helper stdout/stderr. The DBus
consumer prints reference strings in query logs (terminal-dbus.c:50), and its
reference validator accepts nonnumeric suffixes under
`/org/a11y/atspi/accessible/` (lines58–64). A valid path ending `/FAIL` is permitted
by that source-level contract, although it was not observed in any PR42 Qt log.
Library/loader/timeout stderr is not a closed machine-token vocabulary either.

An additional tiny mock supplied a correctly shaped DBUS_A11Y_QUERY line with
that source-accepted `/FAIL` path in an earlier rc1 attempt, followed by a normal
successful attempt. The production shell loop returned0, but retained history
matched the unchanged host sentinel, so the host would reject it. This is a
bounded demonstration using a legal reference field, not evidence that current
Qt emitted such a path, nor a new real-guest/cold failure. It establishes that
shell query-status noninterference is narrower than arbitrary host-output
noninterference. The same raw-string problem may also affect the preexisting
last-query replay if such text appears in the final attempt.

Action/limit: retain the strict host parser; do not strip FAIL tokens or exempt
whole diagnostic regions. No change is required to explain the actual six PR42
service results/current status tokens. If future requirements promise immunity
to arbitrary reference/library text, design and test a sentinel-safe reversible
raw-history transport (e.g. lowercase hex decoded offline) with explicit byte
bounds, without weakening acceptance. No such code change was made here.

## 4. Remaining service/usability and reliability limits

- Fixed virtio disk-root positive smoke, not a general device browser or full
  ObjectManager parser. Manager-interface semantics, size/UUID identity, hotplug,
  multiple devices/media and general mount-list shapes remain unvalidated.
- MountPoints intentionally accepts exactly one root entry; additional legitimate
  mount points fail closed. Separate reads are not an atomic snapshot or a pinned
  service-owner epoch. The helper read cap does not cap dbus-send's output file.
- No mount/unmount/format/poweroff storage operations or privilege-policy widening
  were added or validated. Positive enumeration is not authorization coverage.
- Absent/denied/disconnected/malformed cases remain mocked controls, not actual
  negative-service injections in these guest jobs. No such experiments were run.
- Portal/logind/general desktop integration and sustained real-user workflows are
  not established by these read-only storage checks. A passing fixed Dolphin
  rename/create gate is not a general usability/reliability guarantee.
- KMS job invocations explicitly enable ALPINE_QT_KEY_TRACE=1,
  ALPINE_A11Y_DBUS_VALIDATE=1, and live-dbus; retained history itself labels timing
  perturbation. These are not observer-off cold samples. They do not erase the
  original PR40 cold failure, earlier holds, or prove a startup cure/M40 completion.

## Review execution / reset limitations

Only preserved data reads and temporary mocked-shell subprocesses were used.
Integration worktree was clean at the start and final check. No source was edited.
The requested historical coordination POLICY.txt is absent after reset; its
previous ownership restrictions and the current request were followed. The
checkout is shallow: an attempted base18ef→head parser diff could not run because
the base object is unavailable; no fetch/auth action was taken and no historical
parser-diff conclusion is claimed. Current strict parser behavior was read
verbatim and used in all sentinel checks.

An initial optional lock-map comparison also stopped on nonexistent checked-in
ARM lock files. Inspection established that only x86 locks are checked in, while
ARM/RISC locks are generated and preserved by the workflow; the final comparison
is correctly scoped to x86 and the cross-architecture artifacts are described
as generated signed-runtime maps. This was a review-assumption error, not a guest
or service failure; no fixture/test/parser assertion was weakened to pass it.
