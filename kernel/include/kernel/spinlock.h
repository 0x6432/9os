#pragma once
#include <kernel/arch.h>

typedef struct { volatile int locked; } spinlock_t;
#define SPINLOCK_INIT { 0 }

static inline void spin_lock(spinlock_t *l) {
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED)) arch_cpu_relax();
}
static inline void spin_unlock(spinlock_t *l) { __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE); }
static inline uint64_t spin_lock_irqsave(spinlock_t *l) { uint64_t f = arch_irq_save(); spin_lock(l); return f; }
static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) { spin_unlock(l); arch_irq_restore(f); }
