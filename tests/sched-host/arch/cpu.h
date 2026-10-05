#pragma once
/* Host-only IRQ shims: these tests exercise queue bookkeeping, not interrupt hardware. */
#include <kernel/types.h>
static inline uint64_t arch_irq_save(void) { return 0; }
static inline void arch_irq_restore(uint64_t flags) {}
static inline void arch_irq_enable(void) {}
static inline void arch_irq_disable(void) {}
static inline void arch_cpu_relax(void) {}
static inline void arch_wait_for_interrupt(void) {}