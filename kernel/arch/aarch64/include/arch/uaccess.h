#pragma once
/* aarch64 user-access gate: PSTATE.PAN (ARMv8.1) is set except inside user_access_begin/end. */
#include <kernel/types.h>
#include <arch/cpu.h>
#include <arch/trapframe.h>

extern bool a64_pan, a64_rndr;
#define ARCH_UACCESS_NAME "PAN"
static inline void arch_uaccess_set(bool on) {
    if (!a64_pan) return;
    if (on) __asm__ volatile(".inst 0xd500409f" ::: "memory");   /* msr pan, #0 */
    else __asm__ volatile(".inst 0xd500419f" ::: "memory");      /* msr pan, #1 */
}
static inline bool arch_frame_uaccess(struct trap_frame *f) { return !a64_pan || !(f->pstate & (1u << 22)); }
static inline uintptr_t *arch_frame_pc(struct trap_frame *f) { return (uintptr_t *)&f->pc; }
static inline uint64_t arch_entropy(void) {
    uint64_t v = sysreg_read(cntvct_el0) * 0x9e3779b97f4a7c15ULL;
    if (a64_rndr) {
        uint64_t r, nzcv;
        __asm__ volatile("mrs %0, s3_3_c2_c4_0; mrs %1, nzcv" : "=r"(r), "=r"(nzcv));   /* RNDR */
        if (!(nzcv & (1u << 30))) v ^= r;
    }
    return v;
}
