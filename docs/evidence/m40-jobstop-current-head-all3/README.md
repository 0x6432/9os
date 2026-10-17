# Current-head all3 desktop CI — retained, not a reliability cure

At935bd746cb6ea6d9ed833a5125b8f4465dc31ee7, fast run39031883667 is successful
on all6 base/desktop jobs. Complete actual desktop logs from aarch64/riscv64/x86
are archived. Each has root and UID1000 synthetic snapshot3/3 controls, expanded
job-stop conformance, desktop33/33 and physical KMS Konsole/Dolphin16/16, explicit
live-dbus backend, existing Qt platform-key observer and strict acceptance gates.
All non-x86 execution happened in GitHub CI only, not locally.

Full-source run39031883674 is still pending at this checkpoint. No merge,
cold-reliability cure or M40 completion claim. PR18/dependency holds remain.

A separately queued local one-Linux/one-9os fixed physical experiment was lost
in a second sandbox reset before its outcomes/hash checks could be retrieved or
durably pushed. Its result is UNKNOWN, not a pass, and not silently retried. It
cannot replace the earlier genuine bcdf360 missing-PTY failure. Do not infer
whether either planned VM completed from the successful unrelated CI logs.

Durable local desktop33/33 evidence atf737039 and exact original/fixed job-stop
conformance at8bd9852 are separate scopes; the earlier local setuid-BusyBox
control full archive was lost before upload. Binary evidence branch only: never
merge these archives into product.
