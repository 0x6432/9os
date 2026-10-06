#pragma once
/* x86_64 user-access gate: SMAP (EFLAGS.AC via STAC/CLAC) and SMEP, when the CPU has them. */
#include <kernel/types.h>
#include <arch/trapframe.h>

extern bool x86_smap, x86_smep;
#define ARCH_UACCESS_NAME "SMAP"

static inline void arch_uaccess_set(bool on) {
    if (!x86_smap) return;
    if (on) __asm__ volatile("stac" ::: "memory", "cc");
    else __asm__ volatile("clac" ::: "memory", "cc");
}
/* did the interrupted kernel context have user access open? */
static inline bool arch_frame_uaccess(struct trap_frame *f) { return !x86_smap || (f->rflags & (1u << 18)); }
static inline uintptr_t *arch_frame_pc(struct trap_frame *f) { return (uintptr_t *)&f->rip; }
static inline uint64_t arch_entropy(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t v = ((uint64_t)hi << 32) | lo;
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (c & (1u << 30)) {           /* RDRAND */
        uint64_t r; uint8_t ok;
        for (int i = 0; i < 10; i++) {
            __asm__ volatile("rdrand %0; setc %1" : "=r"(r), "=qm"(ok) :: "cc");
            if (ok) { v ^= r; break; }
        }
    }
    return v;
}
