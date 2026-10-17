# PR36 source x86 actual complete success; one ARM infrastructure recovery
Run39052890218 originalattempt1 finishedFAILURE because ARMjob117454916453
lost hostedrunnercommunication (annotation archived separately; fulljoblog404).
Original x86job117454916454 passed actual139/139, disk24/24 +7/7, both e2fsck,
clean shutdown, audio2/2 +no-card2/2, UP10/10, MLFQ12/12, SMP8progress10/10,
SMP8scheduler/allocator11/11. RISCsource already reviewedpassed. Full x86log
and ORIGINALcompleted-run/jobsJSON retained unchanged, not an ALL3pass.

After entire original run completed and head verified against currentPR36
409ecfc, requested ONE job-only ARM recovery via API. Originalfailure retained;
not a test retry, not wholeworkflow rerun, no source change/no gate waiver.
Recovery pending, no ARM success or fullsourceALL3 claim. M40/coldholds remain.
