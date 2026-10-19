# PR42 current-head CI — originals reviewed

Runtimehead07fda66bd9a7904e77b7f3e1051df243ee1e4d21, draft PR42. No reruns. All completed joblogs and validationZIPs downloaded fresh after sandbox reset; complete originals retained, not green badges alone. Non-validation ISO artifacts deliberately not downloaded.

## Signed prebuilt39168427970 — ALL6 SUCCESS

- Runtime x86_64 job117796202995, aarch64 job117796202720, riscv64 job117796202958: each main63/63 and UP9/9. x86SMP8 additionally41/41.
- epollabitest78checks/0fail and epollrestarttest72checks/0fail root ANDUID1000 on ALL3, againx86SMP8. Strictinstaller/mutation/model/nativecontrols reviewed onx86.
- Desktop x86_64 job117796202967, aarch64 job117796202945, riscv64 job117796202979: each33/33 desktop plus16/16 physicalkeyboard/KonsolePTY/Dolphin.
- New scoped UDiskstypedProperty.Get readbacks accepted by the actual signedpackaged service/printer asUID1000 in both desktop and KMS paths onALL3; acceptance marker appears only after allnewmandatoryreadbacks. This closes the former packaged-format validation gap within the documentedfixedvda/vda1/root scope, NOT generalenumeration/mountauthorization/atomicowner-epoch proof.

## Full source39168428016 — ALL3 SUCCESS

x86_64 job117796230700, aarch64 job117796230622, riscv64 job117796230676:
- Each main157/157; disk24/24+persistence7/7; bothfsck; cleanshutdown; audioPCM/WAV2/2+checker andno-card2/2; UP10/10; MLFQ29/29.
- New epollconsumers78/78 and72/72 root/user onALL3 main+MLFQ. x86SMP8RR progress30/30 includesbothconsumers root/user; separateSMP8MLFQschedulerstress11/11 (not newepollconsumer coverage).
- Release job skipped by PRpolicy, not a failed test.

Expected intentional native negative-control output contains `epollabitest: FAIL five-second process watchdog`; its supervisory harness requires exit124 after5s. The separate stalled-supervisor control requires TimeoutExpired at6s and verifies groupkill/reap, unrelatedchild andsubreaperstate. These expected rejection logs are NOT hidden spontaneous testfailures or deadline waivers. Original inheritedpolicy/model/cleanup failures remain preserved elsewhere.

Allordinarydesktopjobs retain existinginstrumentation/observers. None proves observer-off coldreliability. PR40 cold remainsFAILED, allPR18→42integrationholds unchanged, M40unfinished. No merge/readiness/branchdeletion/newcold dispatch occurred while reviewing thisevidence. Evidencebranch NEVERMERGE.

## Exact PR merge-tree provenance

PR CI checkout3d82350d34b472b40271c571fe88b9702a23ed2b merges runtime07fd with base18ef. A fresh token-free PRmerge fetch confirmed its complete Git tree is IDENTICAL to currentruntime07fda66bd9a7904e77b7f3e1051df243ee1e4d21. TreeSHAs and equality recorded in ci-merge-tree-proof.json. This is not a substitute for joblogs; it links the alreadyreviewed CIcheckout to the exactproducttree. No build/testrerun or productchange.
