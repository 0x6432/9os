#pragma once
/* AArch64 (EL1) CPU helpers. */
#include <kernel/types.h>

#define sysreg_read(r) ({ uint64_t __v; __asm__ volatile("mrs %0, " #r : "=r"(__v) :: "memory"); __v; })
#define sysreg_write(r, v) __asm__ volatile("msr " #r ", %0" :: "r"((uint64_t)(v)) : "memory")
#define isb() __asm__ volatile("isb" ::: "memory")
#define dsb(x) __asm__ volatile("dsb " #x ::: "memory")

static inline uint64_t arch_irq_save(void) {
    uint64_t f;
    __asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(f) :: "memory");
    return f;
}
static inline void arch_irq_restore(uint64_t f) { if (!(f & (1 << 7))) __asm__ volatile("msr daifclr, #2" ::: "memory"); }
static inline void arch_irq_enable(void) { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
static inline void arch_irq_disable(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }
static inline bool arch_irq_enabled(void) { return !(sysreg_read(daif) & (1 << 7)); }
static inline void arch_cpu_relax(void) { __asm__ volatile("yield"); }
/* wfi wakes on a pending interrupt even while masked; unmask afterwards to take it. */
static inline void arch_wait_for_interrupt(void) { __asm__ volatile("wfi; msr daifclr, #2" ::: "memory"); }
