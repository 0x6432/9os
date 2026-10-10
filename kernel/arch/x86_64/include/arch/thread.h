#pragma once
#include <kernel/types.h>

struct arch_thread {
    uint64_t rsp;              /* saved kernel stack pointer */
    uint64_t fs_base;
    uint64_t dr[4], dr6, dr7;  /* ptrace hardware breakpoints/watchpoints (u_debugreg) */
    uint8_t fpu[512] __attribute__((aligned(16)));
};
