/*
 * Preemptive scheduler with a compile-time policy:
 *   CONFIG_SCHED_RR   (default) round robin, fixed 10 ms quantum
 *   CONFIG_SCHED_MLFQ multilevel feedback queue: MLFQ_LEVELS levels, quantum doubles per level,
 *                     demotion after using a level's allotment, periodic boost to the top level.
 * SMP: one global run queue shared by all CPUs. Scheduler state (run queue, sleep list, wait
 * queues, thread states, zombies) is protected by sched_lock, taken with interrupts disabled.
 * The lock is held across the context switch and released by the incoming thread in
 * sched_finish_switch(). A switch drops the big kernel lock of the outgoing thread and the
 * incoming thread retakes its own afterwards (lock order: BKL -> sched_lock), so code that does not
 * need the BKL (lock-free syscalls, idle) can sleep, wake and schedule without it.
 */
#include <kernel/sched.h>
#include <kernel/arch.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/slab.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/time.h>
#include <kernel/errno.h>
#include <kernel/process.h>
#include <kernel/spinlock.h>

#if !defined(CONFIG_SCHED_RR) && !defined(CONFIG_SCHED_MLFQ)
#define CONFIG_SCHED_RR 1
#endif

static struct list_node sleep_list = LIST_INIT(sleep_list);
static struct list_node zombies = LIST_INIT(zombies);
static struct list_node all_threads = LIST_INIT(all_threads);
static struct kmem_cache *thread_cache;
static int next_tid = 1;
static int nr_runnable;
static spinlock_t sched_lock = SPINLOCK_INIT;
static volatile int sched_owner = -1;          /* CPU holding sched_lock (debugging) */
void bkl_drop_for_switch(struct thread *t);
void bkl_retake_after_switch(struct thread *t);
/* interrupts must already be disabled */
static void sl_lock(void) { spin_lock(&sched_lock); sched_owner = this_cpu()->id; }
static void sl_unlock(void) { sched_owner = -1; spin_unlock(&sched_lock); }
static uint64_t sl_lock_irqsave(void) { uint64_t f = arch_irq_save(); sl_lock(); return f; }
static void sl_unlock_irqrestore(uint64_t f) { sl_unlock(); arch_irq_restore(f); }

/* ---------------------------------------------------------------- policy */
#ifdef CONFIG_SCHED_MLFQ
static struct list_node queues[MLFQ_LEVELS];
static uint64_t last_boost;

const char *sched_policy_name(void) { return "mlfq"; }
static int quantum_for(struct thread *t) { return MLFQ_BASE_QUANTUM << t->level; }
static void rq_init(void) { for (int i = 0; i < MLFQ_LEVELS; i++) list_init(&queues[i]); }
static void rq_add(struct thread *t) { list_add_tail(&queues[t->level], &t->run_node); }
static struct thread *rq_pick(void) {
    for (int i = 0; i < MLFQ_LEVELS; i++)
        if (!list_empty(&queues[i])) {
            struct thread *t = list_first(&queues[i], struct thread, run_node);
            list_del(&t->run_node);
            return t;
        }
    return nullptr;
}
/* the running thread used up its allotment at this level */
static void quantum_expired(struct thread *t) {
    if (t->level < MLFQ_LEVELS - 1) t->level++;
    t->quantum = quantum_for(t);
}
/* should a newly woken thread preempt the running one? */
static bool wake_preempts(struct thread *woken, struct thread *running) { return woken->level <= running->level; }
/* rule 5: every MLFQ_BOOST_MS move everything back to the top queue */
static void policy_tick(void) {
    if (jiffies - last_boost < MLFQ_BOOST_MS) return;
    last_boost = jiffies;
    for (int i = 1; i < MLFQ_LEVELS; i++)
        list_for_each_safe(it, tmp, &queues[i]) { list_del(it); list_add_tail(&queues[0], it); }
    list_for_each(it, &all_threads) {
        struct thread *t = list_entry(it, struct thread, all_node);
        if (t->level) { t->level = 0; t->quantum = quantum_for(t); }
    }
}
static void pick_reset_quantum(struct thread *t) { if (t->quantum <= 0) t->quantum = quantum_for(t); }
#else
static struct list_node run_queue;

const char *sched_policy_name(void) { return "round-robin"; }
static int quantum_for(struct thread *t) { return SCHED_QUANTUM; }
static void rq_init(void) { list_init(&run_queue); }
static void rq_add(struct thread *t) { list_add_tail(&run_queue, &t->run_node); }
static struct thread *rq_pick(void) {
    if (list_empty(&run_queue)) return nullptr;
    struct thread *t = list_first(&run_queue, struct thread, run_node);
    list_del(&t->run_node);
    return t;
}
static void quantum_expired(struct thread *t) {}
static bool wake_preempts(struct thread *woken, struct thread *running) { return true; }
static void policy_tick(void) {}
static void pick_reset_quantum(struct thread *t) { t->quantum = quantum_for(t); }
#endif

/* ---------------------------------------------------------------- run queue */
static void enqueue(struct thread *t) {
    t->state = T_RUNNABLE;
    rq_add(t);
    nr_runnable++;
}

static struct thread *dequeue(void) {
    struct thread *t = rq_pick();
    if (t) nr_runnable--;
    return t;
}

int sched_runnable_count(void) { return nr_runnable; }

/* A thread became runnable: wake an idle CPU, or preempt this one if the policy says so. */
static void kick_after_wake(struct thread *t) {
    struct cpu *self = this_cpu();
    for (int i = 0; i < ncpus; i++) {
        struct cpu *c = &cpus[i];
        if (c != self && c->online && c->cur == c->idle && !(c->ipi_pending & IPI_RESCHED)) {
            smp_send_resched(c);
            return;
        }
    }
    if (self->cur == self->idle || !self->cur || wake_preempts(t, self->cur)) self->resched = true;
}

struct thread *thread_alloc(const char *name) {
    struct thread *t = kmem_cache_alloc(thread_cache);
    if (!t) return nullptr;
    memset(t, 0, sizeof *t);
    struct page *stk = page_alloc(KSTACK_ORDER);
    if (!stk) { kmem_cache_free(thread_cache, t); return nullptr; }
    t->kstack = PHYS_TO_VIRT(page_to_phys(stk));
    t->tid = next_tid++;
    strlcpy(t->name, name, sizeof t->name);
    t->level = 0;
    t->quantum = quantum_for(t);
    t->bkl_depth = 0;
    t->bkl_saved = 1;        /* new threads take the BKL in sched_finish_switch() before running */
    t->cpu = this_cpu();
    list_init(&t->run_node);
    list_init(&t->timer_node);
    list_init(&t->proc_node);
    list_add_tail(&all_threads, &t->all_node);
    t->state = T_BLOCKED;
    return t;
}

void thread_start(struct thread *t) {
    uint64_t f = sl_lock_irqsave();
    enqueue(t);
    kick_after_wake(t);
    sl_unlock_irqrestore(f);
}

/* Kernel threads draw TIDs from a range above PID_MAX so user PIDs (init = 1) are unaffected. */
static int next_ktid = 1 << 22;
struct thread *thread_create(const char *name, void (*fn)(void *), void *arg) {
    int saved = next_tid;
    struct thread *t = thread_alloc(name);
    next_tid = saved;
    if (!t) return nullptr;
    t->tid = next_ktid++;
    arch_thread_init(t, fn, arg);
    thread_start(t);
    return t;
}

void thread_free(struct thread *t) {
    list_del(&t->all_node);
    pmm_free_pages(VIRT_TO_PHYS(t->kstack), KSTACK_ORDER);
    kmem_cache_free(thread_cache, t);
}

/* idle loop, interrupts off, BKL not held */
static void reap_zombies(void) {
    if (list_empty(&zombies)) return;
    struct list_node dead = LIST_INIT(dead);
    sl_lock();
    list_for_each_safe(it, tmp, &zombies) {
        struct thread *t = list_entry(it, struct thread, run_node);
        if (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) continue;   /* still switching out */
        list_del(&t->run_node);
        list_add_tail(&dead, &t->run_node);
    }
    sl_unlock();
    if (list_empty(&dead)) return;
    bkl_enter();                                  /* slab + all_threads are BKL-protected */
    list_for_each_safe(it, tmp, &dead) {
        struct thread *t = list_entry(it, struct thread, run_node);
        list_del(&t->run_node);
        thread_free(t);
    }
    bkl_exit();
}

/* Must be called with interrupts disabled and sched_lock held; returns with sched_lock released
 * (and the caller's BKL depth restored). */
[[gnu::noinline]] static void __schedule(void) {
    struct cpu *c = this_cpu();
    struct thread *prev = c->cur, *next;
    if (sched_owner != c->id || prev != current)
        panic("__schedule: cpu%d sched_lock owner %d prev %s current %s from %p", c->id, sched_owner, prev->name,
              current->name, __builtin_return_address(0));
    bkl_drop_for_switch(prev);
    c->resched = false;
    if (prev->state == T_RUNNING && prev != c->idle) enqueue(prev);
    next = dequeue();
    if (!next) next = c->idle;
    next->state = T_RUNNING;
    pick_reset_quantum(next);
    if (next == prev) { sched_finish_switch(); return; }
    if (next->on_cpu) {          /* cannot happen under the BKL; kept as a safety net for finer locking */
        while (__atomic_load_n(&next->on_cpu, __ATOMIC_ACQUIRE)) arch_cpu_relax();
    }
    next->on_cpu = 1;
    next->cpu = c;
    c->cur = next;
    c->ctx_switches++;
    c->prev = prev;
    arch_set_current(next);
    arch_switch_to(prev, next);
    sched_finish_switch();
}

/* runs on the new thread right after a switch: the previous thread's context is now saved */
void sched_finish_switch(void) {
    struct cpu *c = this_cpu();
    if (c->prev) { __atomic_store_n(&c->prev->on_cpu, 0, __ATOMIC_RELEASE); c->prev = nullptr; }
    sl_unlock();
    bkl_retake_after_switch(current);
}

void schedule(void) {
    uint64_t f = sl_lock_irqsave();
    __schedule();
    arch_irq_restore(f);
}

void sched_yield(void) {
    if (current) schedule();
    else arch_cpu_relax();
}

/*
 * Lock-free part of the tick on secondary CPUs (called before taking the BKL, interrupts off).
 * Returns true if the tick is fully handled; false means the caller must take the BKL and run
 * timer_tick() (accounting is not repeated).
 */
bool sched_tick_fast(bool from_user) {
    struct cpu *c = this_cpu();
    if (c->id == 0 || !c->cur) return false;
    c->ticks++;
    c->tick_accounted = true;
    struct thread *cur = c->cur;
    if (cur == c->idle) {
        c->idle_ticks++;
        if (nr_runnable) { c->resched = true; return false; }
        c->tick_accounted = false;
        return true;
    }
    cur->run_ticks++;
    if (cur->proc) __atomic_fetch_add(&cur->proc->utime_ticks, 1, __ATOMIC_RELAXED);
    if (--cur->quantum <= 0) { quantum_expired(cur); c->resched = true; return false; }
    if (from_user && (signal_pending(cur) || (cur->proc && cur->proc->alarm_ns))) return false;
    if (c->resched) return false;
    c->tick_accounted = false;
    return true;
}

void timerfd_tick(uint64_t now);
void sched_tick(void) {
    struct cpu *c = this_cpu();
    if (c->id == 0) {
        uint64_t now = time_ns();
        timerfd_tick(now);
        sl_lock();
        list_for_each_safe(it, tmp, &sleep_list) {
            struct thread *t = list_entry(it, struct thread, timer_node);
            if (t->wake_ns <= now) {
                list_del(&t->timer_node);
                t->timer_active = false;
                t->timed_out = true;
                list_del(&t->run_node);
                enqueue(t);
                kick_after_wake(t);
            }
        }
        policy_tick();
        sl_unlock();
    }
    if (c->tick_accounted) { c->tick_accounted = false; return; }
    c->ticks++;
    struct thread *cur = c->cur;
    if (!cur) return;
    if (cur == c->idle) {
        c->idle_ticks++;
        if (nr_runnable) c->resched = true;
        return;
    }
    cur->run_ticks++;
    if (cur->proc) __atomic_fetch_add(&cur->proc->utime_ticks, 1, __ATOMIC_RELAXED);
    if (--cur->quantum <= 0) {
        quantum_expired(cur);
        c->resched = true;
    }
}

void thread_wake(struct thread *t) {
    uint64_t f = sl_lock_irqsave();
    if (t->state == T_BLOCKED || t->state == T_SLEEPING) {
        list_del(&t->run_node);
        if (t->timer_active) { list_del(&t->timer_node); t->timer_active = false; }
        enqueue(t);
        kick_after_wake(t);
    }
    sl_unlock_irqrestore(f);
}

void thread_interrupt(struct thread *t) {
    uint64_t f = sl_lock_irqsave();
    if (t->state == T_BLOCKED || t->state == T_SLEEPING) {
        t->interrupted = true;
        list_del(&t->run_node);
        if (t->timer_active) { list_del(&t->timer_node); t->timer_active = false; }
        enqueue(t);
        kick_after_wake(t);
    }
    sl_unlock_irqrestore(f);
}

void sleep_ns(uint64_t ns) {
    uint64_t f = sl_lock_irqsave();
    current->wake_ns = time_ns() + ns;
    current->state = T_SLEEPING;
    current->timer_active = true;
    list_add_tail(&sleep_list, &current->timer_node);
    __schedule();
    arch_irq_restore(f);
}

[[gnu::weak]] bool signal_pending(struct thread *t) { return false; }

/* The signal check happens under sched_lock so a concurrent thread_interrupt() cannot be missed. */
uint64_t sched_wait_lock(void) { return sl_lock_irqsave(); }
void sched_wait_unlock(uint64_t f) { sl_unlock_irqrestore(f); }

int wait_event(struct wait_queue *q) { return wait_event_locked(q, sl_lock_irqsave()); }

/* caller holds sched_lock (from sched_wait_lock(), flags f) and has checked its condition */
int wait_event_locked(struct wait_queue *q, uint64_t f) {
    if (signal_pending(current)) { sl_unlock_irqrestore(f); return -EINTR; }
    current->state = T_BLOCKED;
    current->interrupted = false;
    list_add_tail(&q->head, &current->run_node);
    __schedule();
    bool intr = current->interrupted;
    arch_irq_restore(f);
    return intr ? -EINTR : 0;
}

int wait_event_timeout(struct wait_queue *q, uint64_t ns) {
    return wait_event_timeout_locked(q, ns, sl_lock_irqsave());
}

int wait_event_timeout_locked(struct wait_queue *q, uint64_t ns, uint64_t f) {
    if (ns == UINT64_MAX) return wait_event_locked(q, f);
    if (signal_pending(current)) { sl_unlock_irqrestore(f); return -EINTR; }
    current->state = T_BLOCKED;
    current->interrupted = false;
    current->timed_out = false;
    current->wake_ns = time_ns() + ns;
    current->timer_active = true;
    list_add_tail(&sleep_list, &current->timer_node);
    list_add_tail(&q->head, &current->run_node);
    __schedule();
    int r = current->interrupted ? -EINTR : current->timed_out ? -ETIMEDOUT : 0;
    arch_irq_restore(f);
    return r;
}

static void wake_thread_locked(struct thread *t) {
    list_del(&t->run_node);
    if (t->timer_active) { list_del(&t->timer_node); t->timer_active = false; }
    enqueue(t);
    kick_after_wake(t);
}

void wake_up(struct wait_queue *q) {
    uint64_t f = sl_lock_irqsave();
    list_for_each_safe(it, tmp, &q->head) {
        struct thread *t = list_entry(it, struct thread, run_node);
        wake_thread_locked(t);
    }
    sl_unlock_irqrestore(f);
}

void wake_up_one(struct wait_queue *q) {
    uint64_t f = sl_lock_irqsave();
    if (!list_empty(&q->head)) {
        struct thread *t = list_first(&q->head, struct thread, run_node);
        wake_thread_locked(t);
    }
    sl_unlock_irqrestore(f);
}

__noreturn void thread_exit(void) {
    arch_irq_disable();
    sl_lock();
    current->state = T_ZOMBIE;
    list_add_tail(&zombies, &current->run_node);
    __schedule();
    panic("zombie thread rescheduled");
}

/* Idle threads run without the BKL (bkl_depth 0); interrupt handlers taken here acquire it. */
static void idle_loop(void *arg) {
    for (;;) {
        arch_irq_disable();
        reap_zombies();
        if (nr_runnable) { sl_lock(); __schedule(); arch_irq_enable(); continue; }
        arch_wait_for_interrupt();
        arch_irq_enable();
    }
}

static struct thread *make_idle(struct cpu *c) {
    struct thread *t = thread_alloc("idle");
    t->tid = 0;
    t->cpu = c;
    t->bkl_saved = 0;
    arch_thread_init(t, idle_loop, nullptr);
    t->state = T_RUNNABLE;
    return t;
}

/* The boot context becomes the first thread ("kmain"); an idle thread is created. */
void sched_init(void) {
    thread_cache = kmem_cache_create("thread", sizeof(struct thread), 64);
    rq_init();
    struct cpu *c = &cpus[0];
    struct thread *boot = kmem_cache_alloc(thread_cache);
    memset(boot, 0, sizeof *boot);
    boot->tid = 0;
    strlcpy(boot->name, "kmain", sizeof boot->name);
    boot->state = T_RUNNING;
    boot->quantum = quantum_for(boot);
    boot->cpu = c;
    list_init(&boot->run_node);
    list_init(&boot->timer_node);
    list_init(&boot->proc_node);
    list_add_tail(&all_threads, &boot->all_node);
    c->cur = boot;
    boot->on_cpu = 1;
    arch_set_current(boot);
    boot->bkl_depth = 0;
    bkl_enter();               /* the boot CPU owns the kernel from here on */
    c->idle = make_idle(c);
    next_tid = 1;              /* first user process (init) gets pid 1 */
#ifdef CONFIG_SCHED_MLFQ
    pr_info("sched: MLFQ, %d levels, quantum %d..%d ms, boost every %d ms\n", MLFQ_LEVELS,
            MLFQ_BASE_QUANTUM, MLFQ_BASE_QUANTUM << (MLFQ_LEVELS - 1), MLFQ_BOOST_MS);
#else
    pr_info("sched: round robin, quantum %d ms\n", SCHED_QUANTUM);
#endif
}

/* Called on the boot CPU (BKL held) before an AP is started. */
void sched_init_ap(struct cpu *c) {
    int tid = next_tid;
    c->idle = make_idle(c);
    next_tid = tid;
}

/* Called on the AP itself with interrupts disabled: become the idle thread. */
__noreturn void sched_start_ap(struct cpu *c) {
    static struct thread dummy[MAX_CPUS];   /* the bootloader stack context, never resumed */
    struct thread *d = &dummy[c->id];
    d->cpu = c;
    d->state = T_ZOMBIE;
    c->cur = d;
    arch_set_current(d);
    sl_lock();                 /* released by the idle thread in sched_finish_switch() */
    c->online = true;
    c->cur = c->idle;
    c->prev = d;
    c->idle->on_cpu = 1;
    c->idle->state = T_RUNNING;
    arch_set_current(c->idle);
    arch_switch_to(d, c->idle);
    panic("AP boot context resumed");
}

void trap_exit_hook_sched(void) {
    if (need_resched && current) { sl_lock(); __schedule(); }
}
