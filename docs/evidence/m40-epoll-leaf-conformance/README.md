# Fixed direct leaf epoll candidate conformance — x86 core only

Product cdfd3a5. Plan542065f committed AND pushed before any VM.
ONE sequential VM each SMP1/SMP4/SMP8, same512M frozenimage, sametwoselected
hostCPUs. Every sample accepted25/25; originalbefore/afterhashes andentire-root
manifest match, andwhole-serieshashesmatch. OriginalfrozenfixtureZIPretained.

Each VM: extendedpollfiltertest199checks0fail root AND UID1000; extended
unixpolltest498checks0fail root AND UID1000; eightbenchmarkmodes root/user,
socktest/ipctest/polltest, boot/lockdep0. No retries, overlaps, deadlinechanges,
performance thresholds oractivefixturemutation. Completeoriginaldrivers/logs,
results/manifests retained. Benchmarkbinary byte-identical toPR39baseline.

This host was rebuilt after a sandboxwipe: NOT a matched-host performance
comparison with priorbaseline, no statisticalrateclaim. CORE ONLY, notfull
Qt/Mesa/LLVM/Sway regression orcoldreliabilityproof. No localnonx86build/test.
Current-head ALL3 PRCI andintegrationholdremain; M40unfinished. Nevermerge
evidence/manual-validationbranches. No newcoldcomparison orretry dispatched.
