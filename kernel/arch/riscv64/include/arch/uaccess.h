#pragma once
/* riscv64 user-access gate: SSTATUS.SUM is clear except inside user_access_begin/end. */
#include <kernel/types.h>
#include <arch/cpu.h>
#include <arch/trapframe.h>

#define ARCH_UACCESS_NAME "SUM"
static inline void arch_uaccess_set(bool on) {
    if (on) csr_set(sstatus, SSTATUS_SUM);
    else csr_clear(sstatus, SSTATUS_SUM);
}
static inline bool arch_frame_uaccess(struct trap_frame *f) { return f->sstatus & SSTATUS_SUM; }
static inline uintptr_t *arch_frame_pc(struct trap_frame *f) { return (uintptr_t *)&f->sepc; }
static inline uint64_t arch_entropy(void) {
    uint64_t t; __asm__ volatile("rdtime %0" : "=r"(t));
    return t * 0x9e3779b97f4a7c15ULL ^ (uint64_t)&t;
}
