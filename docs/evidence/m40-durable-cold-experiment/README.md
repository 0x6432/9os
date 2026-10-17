# Fixed durable cold experiment — validation only, not a cure claim

Run: https://github.com/0x6432/9os/actions/runs/39035386400
Validation branch head: `58f26ebc9c3ee938bf5f54fe140eac65c19603fb`.
Runtime base: PR35 `935bd746cb6ea6d9ed833a5125b8f4465dc31ee7`.
The separate validation branch replaces its workflow and MUST NOT be merged into
product. No kernel/userland/readiness runtime difference. One Linux control and
one dependent 9os sample; no retries. Signed Alpine binaries; tiny consumers and
kernel only compiled. Qt/Wayland key observers OFF; failure-only snapshots ON.

## Completed Linux control
All17 applicable gates passed; the sole retained failure is missing9os lockdep.
Both fixture-integrity checks true; raw physical capture390 events/262 key
edges/128 taps, no drops or explicit repeats. Root/user snapshot3/3 each,
genuine PTY/focused-terminal and Dolphin gates. This is Linux control evidence,
not 9os acceptance, general startup reliability or M40 completion.
Archive is the original complete GitHub artifact12101839224, SHA256 `9586a077efd0a1001e573a9ef1c3bfa5dbe2a7aa2bea3e5cd41f64cbfef90759`.

## Completed 9os sample
The single planned fresh 9os boot passed18/18, driver exit0, raw-check0, immutable
fixture checks true before and after. Root/user final snapshot selftest3/3 each;
explicit live-dbus backend, genuine focused terminal, actual UID1000 PTY physical
typing, genuine Dolphin directory rename/create and filename pixels, clean exit,
lockdep0. Independent visual review of guest1-dolphin.png confirms after.txt and
new.txt on the real application window. Raw stream390 events/262 key edges/128
taps; no drops or explicit repeats. Qt/Wayland key observers were OFF.
Original full GitHub artifact12102321029 SHA256
`bf99a6510a24d1d1bca4dc6c05d71048f4001a0ba40c32d54cf6d56818b82903`.

One accepted guest sample is NOT a cold-start reliability cure, does not erase
previous real failures, and does not complete M40. Dependency/integration holds
remain. Full-source PR35 gates remain pending. The earlier local experiment lost
to reset remains UNKNOWN; this durable plan does not reconstruct its outcome.
