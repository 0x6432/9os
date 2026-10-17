# Connected Unix wake candidate: fixed x86 conformance series
Runtime/source 3a90eed, one predeclared VM each SMP1/SMP4/SMP8 sequentially,
512M core-only, same two selected host CPUs, SAME frozen image and binaries.
All three accepted18/18. New exact unixpolltest274 checks0fail as root AND
UID1000 in each; all six benchmark commands, root/user pollfiltertest,
socktest/ipctest/polltest, boot and lockdep0 pass. Every before/after fixture
hash matches. Complete original logs, driver, adapter, results and native
controls retained. No retries or overlapping VMs. Native216checks0fail.

This is NOT full graphics regression, ARM/RISC validation, desktop causal
proof or a statistical performance claim. Cold-start failures/holds remain.
No acceptance deadline, keyboard input, scheduler policy or GUI gate changed.
Only x86 was built/tested locally; Qt/Mesa/LLVM libraries were not rebuilt.
