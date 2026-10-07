#pragma once
#include <kernel/arch.h>

/*
 * Lock classes and the debug lock-order checker (core/lockdep.c). A classified lock has a
 * rank; a CPU/thread may only acquire locks in strictly increasing rank order (same class
 * only if the class allows nesting). Violations are reported once per pair and counted in
 * /proc/lockdep. Unclassified locks (cls == nullptr) are not checked.
 *
 * Global order (outer -> inner), see docs/HANDOFF.md "Lock order":
 *   BKL -> sleeping mutexes (LR_MUTEX_*) -> fd table -> futex buckets -> pipe -> tty -> unix
 *   -> mm->lock (user copies, faults) -> tmpfs page array -> inode i_mmap -> page-cache LRU
 *   -> page tables -> slab -> buddy/PCP -> block queue -> block driver -> sched (sched_lock, rq locks) -> console
 * Trylocks (spin_trylock/mutex_trylock/mm_trylock) are exempt from the order check: page
 * reclaim takes mm->lock with a trylock while holding the i_mmap lock.
 */
struct lock_class { const char *name; int rank; bool nest; };
enum {
    LR_MUTEX_TTY = 10, LR_MUTEX_EPOLL = 12, LR_MUTEX_VFS = 14, LR_MUTEX_INODE = 16, LR_MUTEX_BMAP = 17, LR_MUTEX_ICACHE = 18, LR_MUTEX_FSALLOC = 19, LR_MUTEX_MISC = 20,
    LR_FD = 30, LR_FUTEX = 32, LR_PIPE = 34, LR_TTY = 36, LR_UNIX = 38, LR_MM = 40,
    LR_ICACHE = 43, LR_PAGECACHE = 44, LR_I_MMAP = 45, LR_LRU = 46, LR_DIRTYLIST = 47, LR_PT = 50,
    LR_SLAB_REG = 54, LR_SLAB = 56, LR_BUDDY_DRAIN = 58, LR_PCP = 60, LR_BUDDY = 62, LR_BLKQ = 64, LR_BLKDRV = 66, LR_CRED = 68,
    LR_SCHED = 70, LR_RQ = 72, LR_CONSOLE = 90,
};

typedef struct { volatile int locked; const struct lock_class *cls; } spinlock_t;
#define SPINLOCK_INIT { 0, nullptr }
#define SPINLOCK_INIT_CLASS(c) { 0, (c) }
static inline void spin_lock_init_class(spinlock_t *l, const struct lock_class *c) { l->locked = 0; l->cls = c; }

void lockdep_acquire(const struct lock_class *c, bool sleeping);
void lockdep_release(const struct lock_class *c, bool sleeping);
void lockdep_acquire_try(const struct lock_class *c, bool sleeping);   /* successful trylock: no order check */

static inline void spin_lock(spinlock_t *l) {
    if (l->cls) lockdep_acquire(l->cls, false);
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED)) arch_cpu_relax();
}
static inline bool spin_trylock(spinlock_t *l) {
    if (__atomic_load_n(&l->locked, __ATOMIC_RELAXED) || __atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) return false;
    if (l->cls) lockdep_acquire_try(l->cls, false);
    return true;
}
void spin_lock_ipi(spinlock_t *l);   /* IRQs off; services TLB IPIs while spinning (core/smp.c) */
static inline void spin_unlock(spinlock_t *l) {
    if (l->cls) lockdep_release(l->cls, false);
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}
static inline uint64_t spin_lock_irqsave(spinlock_t *l) { uint64_t f = arch_irq_save(); spin_lock(l); return f; }
static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) { spin_unlock(l); arch_irq_restore(f); }
