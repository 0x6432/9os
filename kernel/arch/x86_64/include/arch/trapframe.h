#pragma once
#include <kernel/types.h>

struct trap_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;   /* pushed by CPU */
};

static inline bool trap_from_user(struct trap_frame *f) { return (f->cs & 3) == 3; }
