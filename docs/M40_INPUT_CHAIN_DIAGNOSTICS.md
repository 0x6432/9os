# Opt-in M40 input-chain diagnostics

`ALPINE_INPUT_CHAIN_DIAGNOSTICS=1 scripts/alpine-kms-tests.sh x86_64` is a
diagnostic-only mode. The default remains off. It passes the existing outer
Wayland, inner libwayland, and Qt platform-submission observers through the
root-to-user probe boundary, starts the existing non-grabbing evdev collector
only after both genuine readiness markers and the pre-type screenshot, and
arranges to retain their streams in one unique host log for that QEMU attempt.
The before/after/Dolphin screenshots use the same attempt-specific basename.
Streams not returned before a panic/VM exit make the attempt incomplete, as
described below.

The raw collector runs for a fixed 45 seconds. Its helper publishes the status
file only after the bounded collector exits; it has no early-stop control.
After the pre-type screenshot, the diagnostic path makes at most 10 one-second
poll waits for exactly one valid `INPUT_CAPTURE_READY` record and a matching
set of opened-device records. Each inspection snapshots at most 64 KiB of the
growing collector log, checks inspector exit status and output, and checks the
collector's atomic completion-status path before accepting readiness. This is
a bounded-poll/processing contract, **not a strict 10-second wall-clock
guarantee**: scheduling and per-inspection work can add elapsed time. The
existing QEMU per-command timeout remains unchanged. The host-side key sender
independently requires exactly one readiness witness followed by exactly one
successful `__RC<n>=0` completion for the helper's actual command index in the
current QEMU attempt log. This sender-side guard is necessary because
`qemu-test.py` continues to later commands after a failed guest command.
Missing, malformed, failed, duplicate, or out-of-order collector startup
therefore cannot authorize the original physical typing command. The
post-gate readback polls at most 600 times with 0.1-second sleeps for the
atomic status, then includes the completed raw log and status in the host log.
This polling count is not a strict wall-clock guarantee.

If a kernel panic or VM exit occurs before the final guest readback, the raw
collector and other guest streams may not be returned to the host log. Such an
attempt is incomplete; this workflow does not promise to retain every guest
stream after a panic/exit, and the checker must fail it as incomplete. The
collector has no stop/readback shortcut that would make this final step
unconditional.

`scripts/input-chain-check.py LOG` requires exactly one mode witness, collector
start/readiness, raw capture, outer trace, inner trace, and Qt trace in one
orchestrated attempt log. It checks the exact fixed key edges at each boundary,
validates raw readiness/device-count consistency and raw ABI numeric ranges,
rejects capture errors/truncation/incomplete records, and requires zero Qt
autorepeat. The outer checker confirms the outer-compositor-to-KWin wire
sequence only; it cannot prove forwarding beyond the proxy. The inner checker
is libwayland pre-listener dispatch; the Qt hook is platform submission. None
proves terminal consumption or explains a later text discrepancy.

The unique log name and checked section boundaries make this an
**orchestrated one-run artifact only**. They do not authenticate the producer
or prove independent same-boot provenance, and they do not detect a
deliberately spliced log. Do not describe a checker pass as cryptographic or
source-authenticated evidence.

The original QMP key sequence, pre-existing readiness evidence, deadlines,
and acceptance commands are unchanged. Diagnostic mode adds a finite-poll
devices-open gate before that same key command; it does not substitute a new
key sequence or readiness threshold. The QEMU acceptance result is saved
separately from the diagnostic checker result: diagnostic success cannot rescue
an original failure. Tracing can perturb timing, and even a complete
trace-enabled pass is not an unchanged-timing cold-reliability sample or a
cause/fix claim. Keep every failed attempt.

This draft does not build missing diagnostic helpers or change dependencies.
The option fails before disk creation if the existing raw collector or Qt
observer is not already installed in the selected desktop root. Install/build
work remains outside this patch and was not run.
