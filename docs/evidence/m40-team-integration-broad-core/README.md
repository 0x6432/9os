# One sequential broad x86 core-only run

Pinned head df0c8e5 (full SHA in head.txt). Exactly one normal scripts/ci-tests.sh x86_64 --boot-timeout300 invocation. No frozen-image adapter, retry, overlapping VM/heavy build or active root/driver/binary mutation. Later07fda66 adds only host harness/test/CI wiring; kernel, userland and guest drivers unchanged, not relabeled as current-head full graphics validation.

Actual controllerrc0:
- Main116/116, including epollabitest78checks/0fail and epollrestarttest72checks/0fail as root and UID1000.
- Disk-root24/24 and persistence7/7; both e2fsck passes; clean disk-root shutdown; logintest passes initramfs and real ext2root.
- Real PCM/WAV2/2 plus waveform checker; absent-card2/2.
- Lockdep assertions pass.

Original controller, all guest/audio logs and WAV preserved. Initial/final kernel and initramfs hashes match. ISO differs legitimately because standard multi-boot suite regenerates CMDLINE images BETWEEN stopped guests. This false ISO equality is recorded, not accepted as a frozen whole-suite pass.

NO_PORTS restoration: core only, not full graphics. Current-head source/signed-prebuilt ALL3 GitHub CI still pending at this checkpoint. No cold cure, M40 completion, merge or integration-hold waiver. Original failed SA_RESTART baseline and PR40 cold remain unchanged on this evidence branch.
