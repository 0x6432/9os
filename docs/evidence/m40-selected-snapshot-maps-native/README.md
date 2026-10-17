# Diagnostic map-selection WIP — native evidence only
Separate branch notion/m40-snapshot-map-selection at7098690, based on409ecfc.
NO PR opened. Not present in PR36 or its active frozen cold comparison.

Original409 snapshot vs candidate on SAME native synthetic fragmented target:
originalrc1/maps256truncated; candidaterc0/maps347/selected2/PC+SPcovered,
32rawstackwords. Bothdetachsignal0 and targetalive/TracerPid0. Native15/15
actual ptrace controls now include over256-map success and over4096-map failure
with explicit truncation.13/13 pure scanner I/O models include malformed,
missing/overlapping/nonexecutingPC/unreadableSP/unterminated/overlong/openerror;
these I/O models are NOT ptrace or GUI observations. Shellfailureintegration2/2.
Strictnativecompile/diffcheck passed. Scanner uses64KiB buffering, <=4096chunks
of1023bytes, emits<=2 exactPC/SP mappings, fails on missing or uncertain data.
Outerfailure-only4-second budget unchanged; no deadline/cold-start cure claimed.

Optional sanitizer compiled but execution BLOCKED by missinglibasan.so.6/static
runtimes; its exacterror retained, NO sanitizer pass. Signedmusl/9os/ALL3 tests
and actual failedGUI snapshots remain UNVERIFIED. Earlier realPCs remain
unattributed: this native observation does NOT retroactively recover their maps.
No frame unwinding, fake module identity, startup fix, merge or M40 completion.
EvidenceZIP SHA256 `e70e9189f1fffc22e57f55d8fdaa2ca2b985841978d74774e39e62b8db9f2aec`. No credentials or desktop library trees.
