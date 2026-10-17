# Explicit live-DBus readiness candidate: fixed-scope trials

Evidence only; do not merge binary archives into product code.
Private runtime based on PR33 c24a22b and unchanged PR32 f9609c4 kernel.
The tiny consumer is byte-identical to the validated native/signed-musl and
all-three-architecture PR33 diagnostic; target SHA256
 d4d0448b2f1ded308e4ee5d107403324c266aedca72756009423cc843f274d6d.

The backend was selected BEFORE each attempt: direct live AT-SPI bulk children
rather than libatspi ChildCount/individual-child calls. It authenticates genuine
application PID, terminal role and FOCUSED/SHOWING/VISIBLE state; per-query eight
seconds and overall120 seconds unchanged. No fallback, timeout extension, focus
request, fake cache, screenshot or PTY focus oracle. Thus these are candidate
backend results, NOT a rerun of the unchanged original readiness gate.

Fixed scope: one signed-Linux control then two x86 9os cold boots. Preserve all
outcomes in results.json, including any failure. Linux lacks9os lockdep; its
explicit missing-lockdep failure remains. All original physical UID1000 PTY,
Dolphin model, fresh physical pixels, clean exit and lockdep gates retained.
Qt/Wayland observers disabled on the application path. Raw hardware capture
proves only that layer, not inner Qt dispatch/application consumption.

Build-free x86-only make adapter and one-command dry-run included. Fixture
hashes checked before/after every stopped VM; snapshot never mutates base disk.
The preceding integrated hash-failed sample is retained separately and is NOT
silently replaced by this different backend trial. Earlier cold failures remain.
These few candidate observations do not prove causality or complete reliability.
The opt-in checked-in shell selection WIP still needs its own current-head PR CI
and integrated validation; do not label private probe trials as those results.
Default product backend remains libatspi. M40 and integration holds remain open.
