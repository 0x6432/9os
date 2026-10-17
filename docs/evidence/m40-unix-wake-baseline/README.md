# Unchanged-kernel x86 Unix wake study — one frozen baseline
Kernel1bb8bc3 unchanged; tinyconsumer WIPc63d67a adds socketpair transport modes.
ONE VM, two verifiedhostCPUs, guestSMP4/512M coreonly, notdesktop/M40reproduction.
All8 boot/command gates pass: originaleventfd modes, Unix modes root/UID1000,
lockdep0. Allbefore/afterkernel/ISO/initramfs/consumer/driver/adapterhashes valid.
NativeLinux fourmodes+fourbadCLI controls pass. No library/nonx86localbuilds.

Measurements are informational: Unix transfers cause substantial waitercontext
switches despite no privateeventfdreadiness, consistentwith known globalsocket
poll_notify broadcast. Values vary by scheduling; no thresholds, rates, causal
startup explanation or kernelchange yet. Same1000transfers/24waiters/45secalarm,
no retries. Completeoriginalcontroller/driver/serial/results retained. This is
not graphical/reliabilityproof and cannoterase any priorfailed/unknowntrial.
A latercandidate MUST retain broad control-plane/dgram/lifetime wake semantics
and validate ordinaryblockingI/O/select/epoll/aliases/close/shutdown/readiness.
Allcold/integrationholds remain and M40unfinished.
