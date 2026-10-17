# Exact six captured PC attribution — NO unwind or root cause
Run39056610852, failedguest2/3 only, all6validboundedselectedmaps. Every ELF is
from the exact runtime desktoplock's SHA256-pinned APK, extracted regularELF
without installation/scripts/librarycompilation. Program-header checked x86ELF64,
uniquely matching executablePT_LOAD offset/range yields loadbias and ELFvaddr;
fileSHA and matching package provenance retained. No attribution of oldtruncated
snapshots or arbitrary rawstackwords. No nearestsymbol/nearestmodule guesses.

Guest2: QtCore0x152093 has NO containing sized exported symbol; QtCore0x356080
is exactly within QProcess::setProgram(QString const&); Konsoleprivate0xa8d50
has NO containing sized exported symbol. Guest3: ICU0x97121 within sizedexported
ubidi_setClassCallback_76; Breeze0x2f4c9 has NO containing sizedexported symbol;
musl0x682e7 has NO containing sizedexported symbol. Objdump may display NEAREST
exported labels (e.g. tss_set+0x12a): these are NOT function identifications.

The lastmuslPC points at aRET immediately after SYSCALL bytes0f05; captured
orig_rax271(ppoll) and rax-4(EINTR), consistent with tracing interruption. It
cannot prove an original missedwake/deadlock/error or stateatreadinessdeadline.
Other fivecaptured orig_rax=-1 userexecution locations. DifferentPCs across
snapshots are post-failure observations, not a processwide consistency guarantee.
No symbol debug info/unwoundframes or inferredcallchain. All samples timing-
perturbed AFTER irrevocablefailure. These results improve observability ONLY.
Full original snapshots/stackwords remain in separate originalartifact archives.
