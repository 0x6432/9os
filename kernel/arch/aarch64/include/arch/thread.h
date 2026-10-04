#pragma once
#include <kernel/types.h>
struct arch_thread {
    uint64_t sp;               /* saved kernel stack pointer */
    uint64_t tpidr;            /* user TLS (TPIDR_EL0) */
    uint64_t fpu[66] __attribute__((aligned(16)));   /* q0-q31, fpsr, fpcr */
};
