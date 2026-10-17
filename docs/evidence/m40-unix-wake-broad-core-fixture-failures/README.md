# Broader local609eb41 core run FAILED; two harness/restoration defects
Original rc1, frozen before/after hashes match, no retry or result overwritten.
Main103/104 (logintest OWN=UNKNOWN), disk21/24 and3/7; both e2fsck pass,
clean shutdown and audio2/2 +no-card2/2. Full original logs/scripts retained.
1) Restored tracked skel passwd/group are0600, and exact frozen cpio shows0600;
nonroot name lookup cannot read accounts. Not a new Unix-wake behavior proof.
2) Controller incorrectly reused the frozen single-image adapter across the
multi-boot ci-tests suite. Its --old-file=ISO suppresses legitimate CMDLINE
repacking, so both intended diskroot boots actually used tmpfs (logs confirm).
Persistence/ext2 gates correctly rejected that wrong root; no kernel disk bug
inferred. Original all failures retained, NOT accepted/full-suite pass.

Next corrected fixture must restore canonical tracked0644/0755 modes, no source
content changes, and use NORMAL make (not frozen run adapter) so the standard
suite can repack CMDLINE images BETWEEN sequential boots. No change to suite
assertions/deadlines, no retry-until-pass. Preserve new outcome separately.
No local Qt/Mesa/LLVM library build, no nonx86 local validation, no M40 cure.
