#pragma once
#include <kernel/types.h>
#include <kernel/list.h>

/* Compact kernel siginfo (M33); siginfo_to_user() expands it to the 128-byte Linux layout
 * according to signo/code. Fields: pid/uid of the sender, i1 = status / overrun / syscall /
 * fd, i2 = timer id / audit arch, v = sigval / fault addr / call addr / band / utime, v2 = stime. */
struct ksiginfo {
    int32_t signo, code, err, pid;
    uint32_t uid;
    int32_t i1, i2, _pad;
    uint64_t v, v2;
};
struct sigq_node { struct list_node node; struct ksiginfo info; };
/* queued signal information of a thread or process; the pending bitmask stays in sig_pending */
struct sigpend {
    struct ksiginfo std[32];      /* standard signals coalesce: one record each (index = signo) */
    struct list_node rtq;         /* real-time signals queue every instance (sigq_node) */
};
