# PR38 e0bc041 corrected restored local x86 core suite: rc0
ONE separately recorded corrected fixture; originalfailed609eb41 suite retained
under m40-unix-wake-broad-core-fixture-failures, never overwritten/reclassified.
Restore canonical tracked0644/0755 and skeleton directory0755 (535modechanges,
fullbefore/after modeinventory retained; sourcecontents unchanged/gitclean), then
normal standardmake for correct CMDLINE diskroot repacking BETWEEN boots, not
frozen single-image adapter. Actualcorrected main104/104,disk24/24+7/7,
both e2fsck, clean diskroot shutdown, PCM/WAV2/2,no-card2/2 allpass; rc0.
Both root/user newunixpolltest274checks0fail and transportbenchmarks pass.
Login allpass in initramfs ANDactual ext2root; actual persistence verification
passes. Zero lockdep assertions pass. Onlyx86 local, no desktoplibrarybuilds.

Standard suite legitimately regenerates ISO/initramfs between sequential boot
configurations, so whole-suite initial/finalimagehashes can differ; recorded
False is NOT a frozen-fixture pass and is not used as acceptance. Rawhashes
retained. No activeVM image mutation or overlapping localVM; normaldriver
rebuilds BEFORE launch. Kernelcode unchanged during suite. This is available
core-only regression, NOT fullQt/Mesa/Sway graphics or desktopcold proof.
All earliercoldfailures and integration/M40holds retained. NoPRmerge.
