#pragma once
#include <kernel/types.h>
/* Saved user/kernel context. regs[i] = x<i> (regs[0] unused). */
struct trap_frame {
    uint64_t regs[32];
    uint64_t sepc, sstatus, scause, stval, orig_a0, _pad;
};
static inline bool trap_from_user(struct trap_frame *f) { return !(f->sstatus & (1UL << 8)); }
