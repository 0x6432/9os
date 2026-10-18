# Completed fixed PR38 cold comparison — FAILURE

Run39063560162, attempt1, validation6046bd2, productruntime e0bc041.
Linux accepted17 applicable; sole expected absent9oslockdep. Guest1FAILED8/18,
guest2ACCEPTED18/18, guest3FAILED8/18. All before/afterfixturechecks valid.
No reruns/additional samples. WholeplanFAILURE, only1/3guestaccepted.

Guest2 rawhardware:390events/262keyedges/128taps, zero repeats/drops/errors;
actualfocusedterminal and UID1000PTY, Dolphin model/capture/rename/create gates
accepted. Normal Qt/Wayland inputobserversOFF throughout fixedplan.

Guest3PTY1/terminal0,pid1040,last8secondqueryalarmed beforeterminalrole.
Threefailure-only snapshotsreturned0, selected2 PC/SPcoverage1+1,
chunks934/935/935,truncated0/error0/malformed0. AllPCsinmappedmusl;
no fixeddeadlock or missedwake proof. Laterfailure-onlyqueryreachedterminalrole
butalarmed inGetState(rc142). Cannotrescueoriginalgate. Guest1failureand
postfailureperturbationremainunchanged. No acceptedphysicalstreamonfailedguests.

Fulloriginalguest2/3ZIPsandcompletejoblogretained. Originalguest1/control/
prerequisiteZIPsnotreplaced. PR18andentireintegrationstackremainheld;
PR38hasNOTsolvedcoldstartup. M40unfinished. Nevermergethisevidencebranch.
