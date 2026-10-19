# Final native control checkpoint

No guest/CI run is claimed. Product PR42/manual runtime stays07fd.

- Original eleven strict command-mock methods:11/11,17.203s.
- Evidence/decoder/failure methods:21/21,10.796s.
- Temporary mutations:9/9 rejected by explicit assertion mismatches.
- Shell syntax and git diff whitespace checks:pass.
- Recovered historical original source/output hashes:6/6 pass.

The recorder-stderr gap control before isolation is retained, as are initial
positive control logs and all original baseline logs. No failure was waived.
The original checker source is byte-for-byte unchanged; its baseline hash is
checked automatically by the evidence suite. Only recorder-runtime stderr is
isolated; original DBus/checker stderr, command/checker statuses, exact readback
acceptance, UID1000, query arguments/count and deadlines remain authoritative.

Final code SHA256:

`b7e6138f346a22f468cb8141d143d842a7dbdcbc925c232a92981158cc225030`  `userland/alpine/desktop-overlay/usr/bin/alpine-udisks-smoke`

`c69ec1e7449aaacc4eac27159059f4461deb13e7cf997b7a5c864f5b5f14d29d`  `userland/alpine/desktop-overlay/usr/share/9os/udisks-readback-evidence.py`

`ae344e61a221d7c0d222cc3a6c711ef9bb639ed44562da5bdd7237d7c3049bc7`  `userland/alpine/desktop-overlay/usr/share/9os/udisks-property-check.py`

`466cab7825a4996e6127fda60c453dd0d3ded3d7c38b676bb2b5a6852a32956a`  `tests/udisks-host/decode-readback-evidence.py`

## Required parent handoff

Independent review and new-head signed guest CI remain required after parent
cohort/pinned-fixture work. Validate both desktop/KMS paths on x86_64/aarch64/
riscv64, preserve actual raw artifacts, decode all three complete small records
and reapply the unchanged typed checker to recovered bytes. Validate status0/0,
typed=passed, recording=ok, completeness and exact identities, not merely record
presence. No retroactive bytes are fabricated for07fd. Parent owns integration,
resource grants, pushes/PR/CI and all remaining cold/reliability/M40 holds.

Recording adds file I/O/Python startup and serial bytes. It perturbs timing;
unchanged deadline constants and return-code preservation are not proof of
identical scheduling or observer-off cold behavior. The documented10752-byte
added-stdout bound excludes preexisting command/checker stderr and final marker.
