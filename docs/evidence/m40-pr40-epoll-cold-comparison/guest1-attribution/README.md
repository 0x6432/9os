# Exact stopped PCs — no unwind, pre-failure state or cause
RuntimeCDF desktop lock SHA-pinned regular ELFs only; no installation/scripts/
library compilation. x86ELF64 executablePT_LOAD uniquely matches mapoffset/range.
Sample1 musl0x55ef7 inside sized exported syscall, captured orig_rax319/rax20.
Sample2 QtCore0x1c6c00 QObject destructor entry, two alias names.
Sample3 musl0x682e7 no containing sized exported symbol, RET afterSYSCALL;
orig_rax271/rax-4 consistent with interrupted ppoll/EINTR during tracing.
Nearest objdump tss_set label is NOT function identification. All are bounded
POST-failure timing-perturbed observations, NOT root cause or original focus.
