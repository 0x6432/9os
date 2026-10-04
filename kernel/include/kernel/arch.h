#pragma once
/* Architecture interface: every port implements these. */
#include <kernel/types.h>

void arch_early_init(void);      /* console, CPU tables, exceptions */
void arch_init(void);            /* after memory management is up: interrupts, timers */
__noreturn void arch_halt_forever(void);
void arch_debug_putc(char c);    /* early serial output */

static inline uint64_t arch_irq_save(void);
static inline void arch_irq_restore(uint64_t flags);
static inline void arch_irq_enable(void);
static inline void arch_irq_disable(void);
static inline void arch_cpu_relax(void);
static inline void arch_wait_for_interrupt(void);

#include <arch/cpu.h>
