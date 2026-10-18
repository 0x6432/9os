# Team development: host controls and independent reviews

Original three isolated feature branches were swapped among agents for independent reviews; parent independently reran native/model/mocked controls and owns all CI/VM/credential operations. Snapshot integrationf7b59c1. Only evidence lives here; NEVER merge this branch.

Parent results: nativeLinux epoll76/76; production-body model119/119 and7/7 mutations rejected; startup history initially12/12, then13/13 after backend-provenance guard; backend5/5, a11y integration5/5, readiness9/9, PTY8/8; scoped UDisks mock11/11; installer exact/missing/duplicate/strictflags controls passed for both newepoll consumers with all earlier controls retained. Exactsignal no_restart classification461/461 in each of two nativevariants and6mutations across11executions rejected. No non-x86 build or realUDisks/graphics validation here.

Review findings retained, not waived:
- Inherited caught-SA_RESTART policy issue: subsequently reproduced once in original x86guest and fixed in a separately recorded changed-head guest (separate evidence folders).
- Startup diagnostic provenance accepted foreign backend prefixes: pre-fix reproduction retained; parent guard and extra regression control added. Acceptance was never altered.
- Nativewatchdog exceptional timeout cleanup left a descendant ifsupervisorstalled: independent intentionalmutation exposed it. Original normal5sconsumerwatchdog pass remains valid, but harnessfix is outstanding at this evidence checkpoint. Initial independent reserved-glibc-signal fixture failure also preserved, not counted as pass.

Native/model/mocks do not prove guest signalframes, packaged UDisks reply format, non-x86 compatibility or cold reliability. PR40 cold failed; all stack holds and M40unfinished remain.
