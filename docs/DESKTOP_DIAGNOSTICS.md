# Desktop readiness and input diagnostics

M40 remains open. These tools observe real applications/input; they do not
replace focused-terminal, physical UID1000 PTY, Dolphin model/pixel, clean-exit
or lockdep gates. A passing cold boot is not general reliability evidence.

## Startup polling correction

The Sway IPC socket can exist before backend initialization finishes. The KMS
input readiness loop already polls up to 100 times, but an unguarded `swaymsg`
under `set -e` aborted the entire probe on its first IPC error. Keep IPC failure
inside that existing conditional loop, require an actual keyboard in JSON, and
stop if Sway exits. The existing poll count/sleep and later gates are unchanged.
`plasma-input-startup-host-tests.py` executes the actual shell block: transient
error, empty input list, permanently missing keyboard, dead compositor, and the
original first-error-abort negative control. This corrects startup observation,
not an established fix for 9os duplicated typing or terminal accessibility stalls.

## Failure-only accessibility phases

`desktopa11y --terminal-trace PID` adds begin/end monotonic receipt timestamps
around genuine libatspi queries. Normal `--terminal PID` does not add trace
clock calls or output. Both modes use identical traversal and focused/showing/
visible terminal criteria. Tracing must not rescue a failed readiness decision.
Mock tests include a blocked role query; a real native Linux observation found
the terminal through libatspi. That does not establish 9os query latency.

## Raw evdev observation

The tiny `inputeventcapture [seconds]` consumer uses public 64-bit Linux evdev
ABI definitions and signed musl, not Linux headers or rebuilt Qt. Duration is
1..120 seconds (default45), with an outer alarm. It opens event devices read-only,
nonblocking, never grabs or injects input, records device identity/clock setup,
all events and receipt time, then prints explicit completion/error/drop totals.

For a disposable test VM, start this only after genuine application readiness:

```sh
input-capture-session /tmp/events.log /tmp/events.status 45 &
```

The status helper publishes via a temporary file and atomic rename **after** the
collector exits. Redirecting the background wrapper directly to the final status
file creates an empty file immediately and is not a completion witness. Actual-
shell controls retain that original bug as a negative and check RC0/RC7 handling.
Do not accept a partial log merely because its status pathname exists.

`input-capture-check.py LOG` compares a complete capture against the exact fixed
QMP typing command, including key/modifier edges and SYN frames. Its synthetic
controls are not hardware proof. A zero-event capture following a correctly
refused readiness gate is complete diagnostic output, not a successful keyboard
comparison. Raw clean input on a passing boot alone does not explain a different
boot's duplicated characters. Correlate layers in the **same failing boot**.

## Outer Wayland observation

`wayland-presentation-proxy --trace-keyboard -- COMMAND ...` optionally records
outer compositor-to-KWin keyboard key/repeat-info/enter/leave messages, tracking
registry binds, seat keyboard creation and object lifetime. Logs are bounded to
4096 records plus an explicit truncation marker. Unknown messages remain
uninterpreted and forwarded. Normal mode has no keyboard observer or trace clock
calls. `ALPINE_PLASMA_KEY_TRACE=1` enables this only in callback presentation mode.

Wire timestamps are source compositor milliseconds; `received_ns` is proxy
receipt time, **not** hardware or inner Qt client delivery latency. Input/repeat
configuration, byte order, SCM_RIGHTS and actual acceptance criteria are not
modified. Host controls cover exact payloads, unrelated objects, lifetime reuse,
bounds, fragmented/short writes and descriptor transfer. Packaged proxy self-
tests exercise traced forwarding as root/non-root in all-architecture PR CI.

## Inner Konsole dispatch observation

`ALPINE_KONSOLE_INPUT_TRACE=1` enables stock `WAYLAND_DEBUG=client` only on
Konsole's interactive launch. The normal command/environment, actual child PID,
focus traversal, deadlines and input repeat settings are unchanged when disabled.
The child setting does not leak back to the probe or its later helper launches.
At exit, `INNER_KEYBOARD_DISPATCH_TRACE_BEGIN/END` frames only `wl_keyboard`
records from the real Konsole log. This is opt-in diagnostic logging and can
perturb timing; it is not an unchanged-fixture reliability comparison.

`inner-keyboard-dispatch-check.py LOG` checks the exact fixed physical typing
sequence at libwayland1.23.1's pre-listener event-dispatch boundary, requiring one
keyboard/queue and consistent repeat settings. It rejects missing/extra/reordered,
malformed, outgoing/discarded events and ambiguous replayed logs. An empty trace
after a correctly refused focus gate is NOT a keyboard pass. A complete correct
sequence is NOT proof that Qt's listener, autorepeat timer or Konsole handled it
correctly. The debug prefix is CLOCK_REALTIME microseconds truncated to uint32;
it wraps and is not a monotonic receipt clock. This checker makes no timing claim.
Synthetic/parser and actual-shell PID/environment controls do not substitute for
same-boot real-client evidence or the original GUI acceptance gates.

## Evidence discipline

Use fresh disposable snapshots, hash fixtures before/after stopped VMs, and run
only one VM at a time. Never mutate an active image/driver/kernel. Preserve old
failures when correcting a fixture; declare fresh samples instead of relabeling
old gates. Linux has no 9os `/proc/lockdep`, so retain that failed non-applicable
check rather than reporting an artificial full pass. Build/test only x86_64
locally; non-x86 execution belongs to current-head GitHub PR CI.

## Qt platform-key submission (opt-in, Qt 6.8.3 LP64 only)

`ALPINE_QT_KEY_TRACE=1 scripts/alpine-kms-tests.sh x86_64` selects an explicit
`konsole-key-trace` child wrapper. It uses a tiny C `LD_PRELOAD` observer for
Qt 6.8.3's exported, versioned `QWindowSystemInterface::handleExtendedKeyEvent`
(timestamp overload). No Qt, Mesa or LLVM library is compiled. Default launches
have no preload, no extra clock calls and no observer output. The wrapper uses
`exec`, preserves the real Konsole PID/arguments and any explicit existing
preload, and does not force `WAYLAND_DEBUG`, keyboard repeat or focus changes.
`ALPINE_KONSOLE_INPUT_TRACE=1` can independently enable the earlier stock
libwayland pre-listener dispatch trace.

The observer forwards opaque `QWindow *` and `QString const &` arguments
untouched. It records before/after monotonic observations, the unchanged source
timestamp, key/native fields, autorepeat flag, count and boolean return. It
limits output to 4096 events and explicitly marks truncation. Non-Qt children
can inherit the preload without a missing-Qt startup failure. This private
C++ ABI is intentionally pinned to the reviewed Qt 6.8.3 LP64 signatures on
x86_64/aarch64/riscv64; do not reuse it for other Qt versions or ABIs without
fresh review and actual-client validation.

`qt-key-observer-selftest` is an independent C forwarding surrogate (4100 calls,
all arguments/return values, bounded output and inherited non-Qt child safety),
not actual Qt, application, scheduler or SMP proof. Host controls additionally
exercise exact recording, wrapper/PID/environment behavior and malformed,
truncated, mixed-process, reordered and ambiguous-replay rejection. All local
builds/tests are x86_64 only; non-x86 target validation belongs to PR CI.

The real Konsole log is framed as `QT_PLATFORM_KEY_TRACE_BEGIN/END` on both
success and failure. Run:

```sh
python3 scripts/qt-key-observer-check.py --require-no-autorepeat build/test-alpine-kms-x86_64.log
```

The checker requires the exact non-repeat physical edge sequence (XKB scan code
is evdev code + 8), complete matching argument/result pairs, one client PID,
monotonic observer timestamps and unambiguous framing. Autorepeat is always
reported, never filtered out of evidence; the explicit flag additionally rejects
any repeat in this fixed-tap test. CI uses that stricter mode. Missing events
on a refused-input boot are a failed diagnostic, not an accepted input sample.

This boundary is **platform event submission, not Qt application/terminal
consumption**. Logging/preloading perturbs timing. A correct trace or passing
boot cannot erase an earlier failure or establish general desktop reliability.
Before portable integration, a private version was demonstrated with the signed
Qt libraries on matching Linux and 9os: Linux all 17 applicable desktop checks
and one 9os 18/18 run, all raw/outer/inner/platform edges exact and no autorepeat.
The other predetermined 9os sample failed before input (PTY/focus not ready,
zero injected/captured keys). Full stopped hashes, logs and screenshots were
retained. The earlier same-boot duplicated character remains unresolved; M40
and the existing cold-desktop integration holds remain open.
