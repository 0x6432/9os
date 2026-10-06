#pragma once
/*
 * Sleeping mutexes (M24). Contended lockers block uninterruptibly on the mutex's wait queue;
 * a context switch drops the BKL, so taking a mutex while holding the BKL cannot deadlock
 * with a mutex holder that wants the BKL. Never take a mutex with IRQs off or while holding
 * a spinlock (lockdep reports it). Not recursive.
 */
#include <kernel/sched.h>
#include <kernel/spinlock.h>

struct mutex {
    int locked;
    int waiters;
    struct thread *owner;
    struct wait_queue wq;
    const struct lock_class *cls;
    uint64_t contended;          /* slow-path acquisitions (statistics) */
};
#define MUTEX_INIT(n, c) { 0, 0, nullptr, WAIT_QUEUE_INIT((n).wq), (c), 0 }

void mutex_init(struct mutex *m, const struct lock_class *c);
void mutex_lock(struct mutex *m);
bool mutex_trylock(struct mutex *m);
void mutex_unlock(struct mutex *m);
static inline bool mutex_owned(struct mutex *m) { return __atomic_load_n(&m->owner, __ATOMIC_RELAXED) == current; }

struct lockdep_stats { uint64_t checks, violations, overflows, mutex_contended; };
void lockdep_get_stats(struct lockdep_stats *s);
int lockdep_report(char *buf, int max);   /* /proc/lockdep text */
