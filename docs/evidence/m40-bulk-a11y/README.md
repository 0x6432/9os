# Independent bulk AT-SPI diagnostic: frozen trials, 2026-10-17

Evidence retention only. Do not merge binary archives into the product branch.
Runtime PR32 f9609c4, private diagnostic source 9c7d258; signed prebuilt
Qt/KDE libraries unchanged, only tiny x86 consumer compiled locally.

Fixed scope: one matching signed-Linux control then two 9os cold boots, no
retries. All frozen fixture hashes verified before and after each stopped VM.
Linux 17/18: all 17 applicable gates pass, missing /proc/lockdep retained.
Both 9os boots 18/18: original focused-terminal readiness, UID1000 physical
PTY command, actual Dolphin file model and fresh physical pixels, cleanup,
zero lockdep. All three boots raw390/262edges/128taps/0repeat/drop/error,
outer262, Qt platform262/0autorepeat. These observers are not application
consumption proof; the separate PTY and model gates are retained.

The independent public GIO/AT-SPI diagnostic observed the real matching Qt PID
and terminal FOCUSED/SHOWING/VISIBLE in all three boots. It ran AFTER the
original readiness gate passed, with an explicit private flag; it cannot rescue
an already failed gate. No normal readiness deadline or criteria changed.
Native and exact signed-musl protocol controls: 23/23 real private-bus cases
and seven CLI bounds each, explicitly synthetic server, not GUI proof.

Extra diagnostic timing means these samples are not unchanged-timing or
cold-start reliability proof. The preceding mixed failure-phase trials and
all earlier failures remain valid. M40 and integration holds remain open.
The failure-only branch was not exercised in this batch because both guests
passed original readiness; do not invent a failure-only observation.
