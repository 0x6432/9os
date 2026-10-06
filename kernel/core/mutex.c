/* Sleeping mutexes, see kernel/mutex.h. */
#include <kernel/mutex.h>
#include <kernel/printk.h>

static uint64_t mutex_contended_total;

void mutex_init(struct mutex *m, const struct lock_class *c) {
    m->locked = 0; m->waiters = 0; m->owner = nullptr; m->cls = c; m->contended = 0;
    wait_queue_init(&m->wq);
}

static bool try_take(struct mutex *m) {
    if (__atomic_exchange_n(&m->locked, 1, __ATOMIC_SEQ_CST)) return false;
    __atomic_store_n(&m->owner, current, __ATOMIC_RELAXED);
    return true;
}

bool mutex_trylock(struct mutex *m) {
    if (!try_take(m)) return false;
    if (m->cls) lockdep_acquire_try(m->cls, true);
    return true;
}

void mutex_lock(struct mutex *m) {
    if (m->cls) lockdep_acquire(m->cls, true);
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == current && current)
        panic("mutex_lock: recursive lock of %s", m->cls ? m->cls->name : "mutex");
    if (try_take(m)) return;
    m->contended++;
    __atomic_fetch_add(&mutex_contended_total, 1, __ATOMIC_RELAXED);
    for (;;) {
        /* waiters is raised before the final try under the sleep lock: mutex_unlock either
         * sees it (and wakes us) or our try sees the lock already released */
        uint64_t f = sched_wait_lock();
        __atomic_fetch_add(&m->waiters, 1, __ATOMIC_SEQ_CST);
        if (try_take(m)) {
            __atomic_fetch_sub(&m->waiters, 1, __ATOMIC_SEQ_CST);
            sched_wait_unlock(f);
            return;
        }
        wait_event_uninterruptible_locked(&m->wq, f);
        __atomic_fetch_sub(&m->waiters, 1, __ATOMIC_SEQ_CST);
        if (try_take(m)) return;
    }
}

void mutex_unlock(struct mutex *m) {
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) != current)
        panic("mutex_unlock: %s not owned by %s", m->cls ? m->cls->name : "mutex", current ? current->name : "?");
    if (m->cls) lockdep_release(m->cls, true);
    __atomic_store_n(&m->owner, nullptr, __ATOMIC_RELAXED);
    __atomic_store_n(&m->locked, 0, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&m->waiters, __ATOMIC_SEQ_CST)) wake_up_one(&m->wq);
}

uint64_t mutex_contended_count(void) { return __atomic_load_n(&mutex_contended_total, __ATOMIC_RELAXED); }
