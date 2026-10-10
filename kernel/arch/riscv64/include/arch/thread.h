#pragma once
#include <kernel/types.h>
/* offsets of sp/ktop/uscratch are used by trap.S */
struct arch_thread {
    uint64_t sp;               /* 0: saved kernel stack pointer */
    uint64_t ktop;             /* 8: top of the kernel stack (user trap frame sits below it) */
    uint64_t uscratch;         /* 16: user sp scratch during trap entry */
    uint64_t fpu[33];          /* f0-f31 + fcsr */
    uint64_t bvr[16], wvr[16]; /* ptrace hardware breakpoints / watchpoints (SBI debug triggers) */
    uint32_t bcr[16], wcr[16];
    bool hwdbg;
};
