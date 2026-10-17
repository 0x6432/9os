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

## Evidence discipline

Use fresh disposable snapshots, hash fixtures before/after stopped VMs, and run
only one VM at a time. Never mutate an active image/driver/kernel. Preserve old
failures when correcting a fixture; declare fresh samples instead of relabeling
old gates. Linux has no 9os `/proc/lockdep`, so retain that failed non-applicable
check rather than reporting an artificial full pass. Build/test only x86_64
locally; non-x86 execution belongs to current-head GitHub PR CI.
