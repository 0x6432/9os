#pragma once
#include <kernel/types.h>
/* Saved context: x0-x30, sp_el0, elr_el1, spsr_el1, esr_el1, far_el1, orig_x0. */
struct trap_frame {
    uint64_t regs[31];
    uint64_t sp, pc, pstate, esr, far, orig_x0, _pad;
};
static inline bool trap_from_user(struct trap_frame *f) { return (f->pstate & 0xf) == 0; }
