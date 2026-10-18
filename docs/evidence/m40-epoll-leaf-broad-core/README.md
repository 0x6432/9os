# Separate standard broad x86 core regression — accepted, NOT frozen whole-ISO

Productcdfd3a5. Plan6afed57 committed AND pushed before launch. ExactlyONE
standard scripts/ci-tests.sh x86_64 --boot-timeout300 execution, normalmake,
sequential boots; no frozenimageadapter, no retries/overlap/activefixturemutation.
Actualmain112/112,disk-root24/24+7/7,bothe2fsck,cleandisk-rootshutdown,
PCM/WAV2/2,no-card2/2. Extendedpollfilter199/unixpoll498 checks0failroot/user.

Driver0. WholefixtureequalityFALSE: ONLYISOchanged, asstandardmultibootsuite
repackagesCMDLINE BETWEEN launches. RecordedFalse isNOTa frozenfixturepass.
Kernel/initramfs/consumer/driver hashes andentire-rootmanifest unchanged. All
originalseriallogs/driveroutput/fsck/manifests/results retained. Originalfrozen
candidatefixture remainsseparatelyretained in m40-epoll-leaf-conformance.

CORE ONLY: no Qt/Mesa/LLVM/GLib librarybuiltlocally, no localnonx86build/test,
NOT full graphicsregression orcoldreliabilityproof. Current-head ALL3CI and
PR18/stackintegrationholdsremain; M40unfinished. Nevermergethisevidencebranch.
