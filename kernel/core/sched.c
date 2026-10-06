/*
 * Preemptive scheduler with a compile-time policy:
 *   CONFIG_SCHED_RR   (default) round robin, fixed 10 ms quantum
 *   CONFIG_SCHED_MLFQ multilevel feedback queue: MLFQ_LEVELS levels, quantum doubles per level,
 *                     demotion after using a level's allotment, periodic boost to the top level.
 * SMP: per-CPU run queues protect local scheduling; sched_lock coordinates
 * waits, wakeups and cross-CPU migration. Only the incoming CPU's rq lock spans
 * a context switch, and sched_finish_switch releases it before retaking the BKL.
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
static void sl_lock(void) { spin_lock_ipi(&sched_lock); sched_owner = this_cpu()->id; }
static void sl_unlock(void) { sched_owner = -1; spin_unlock(&sched_lock); }
static uint64_t sl_lock_irqsave(void) { uint64_t f = arch_irq_save(); sl_lock(); return f; }
static void sl_unlock_irqrestore(uint64_t f) { sl_unlock(); arch_irq_restore(f); }

static int quantum_for(struct thread *t);

/* ---------------------------------------------------------------- policy */
/*
 * Run queues are per CPU (rqs[cpu id]), each guarded by its own lock.
 * A woken thread goes to an idle CPU it may run on (its last CPU first), otherwise back to
 * its last CPU; a CPU whose queue is empty steals from the busiest queue. Each queue has
 * RQ_LEVELS lists (one per MLFQ level; a single list for round-robin).
 */
#ifdef CONFIG_SCHED_MLFQ
#define RQ_LEVELS MLFQ_LEVELS
static uint64_t last_boost;
const char *sched_policy_name(void) { return "mlfq"; }
static int policy_quantum_for(struct thread *t) { return MLFQ_BASE_QUANTUM << t->level; }
static int level_of(struct thread *t) { return t->level; }
/* the running thread used up its allotment at this level */
static void quantum_expired(struct thread *t) {
    if (t->policy == SCHED_FIFO_ || t->policy == SCHED_RR_) { t->quantum = quantum_for(t); return; }
    if (t->level < MLFQ_LEVELS - 1) t->level++;
    t->quantum = quantum_for(t);
}
/* should a newly woken thread preempt the running one? */
static bool policy_wake_preempts(struct thread *woken, struct thread *running) { return woken->level <= running->level; }
static void pick_reset_quantum(struct thread *t) { if (t->quantum <= 0) t->quantum = quantum_for(t); }
static bool policy_skip(struct thread *t) { return false; }
#else
#define RQ_LEVELS 1
const char *sched_policy_name(void) { return "round-robin"; }
static int policy_quantum_for(struct thread *t) { return SCHED_QUANTUM; }
static int level_of(struct thread *t) { return 0; }
static void quantum_expired(struct thread *t) {}
static bool policy_wake_preempts(struct thread *woken, struct thread *running) { return true; }
/*
 * Deficit round robin: a slice that overran (ticks are coarse, and may be lost under
 * emulation) leaves t->quantum negative; the debt is paid back from the next refill, and a
 * thread still in debt after its refill gives up that turn. Keeps nice shares proportional.
 */
static void pick_reset_quantum(struct thread *t) { if (t->quantum <= 0) t->quantum += quantum_for(t); if (t->quantum <= 0) t->quantum = 1; }
static bool policy_skip(struct thread *t) {
    if (t->quantum > 0) return false;
    t->quantum += quantum_for(t);
    return t->quantum <= 0;
}
#endif

/*
 * SCHED_FIFO/SCHED_RR threads sit on a separate list ordered by rt_prio (FIFO within a
 * priority) that is always served first; they are only preempted by higher RT priorities.
 * Normal threads get a slice scaled by nice (x1.25 per step, 1..100 ms).
 */
static bool is_rt(struct thread *t) { return t->policy == SCHED_FIFO_ || t->policy == SCHED_RR_; }
static int quantum_for(struct thread *t) {
    if (t->policy == SCHED_FIFO_) return 1 << 30;
    if (t->policy == SCHED_RR_) return SCHED_RR_QUANTUM;
    int q = policy_quantum_for(t) * 64;      /* fixed point, 1/64 tick */
    for (int n = t->nice; n > 0; n--) q = q * 4 / 5;
    for (int n = t->nice; n < 0; n++) q = q * 5 / 4;
    q /= 64;
    return q < 1 ? 1 : q > 100 ? 100 : q;
}
static bool wake_preempts(struct thread *woken, struct thread *running) {
    if (is_rt(running)) return is_rt(woken) && woken->rt_prio > running->rt_prio;
    if (is_rt(woken)) return true;
    return policy_wake_preempts(woken, running);
}

struct rq {
    spinlock_t lock;
    int owner;
    struct list_node rt, q[RQ_LEVELS];
    int nr, nr_mig;
    uint64_t steals, balances, local_schedules, coordinated_schedules;
} __attribute__((aligned(64)));
static struct rq rqs[MAX_CPUS];
static void rq_lock(int cpu) { spin_lock_ipi(&rqs[cpu].lock); rqs[cpu].owner = this_cpu()->id; }
static void rq_unlock(int cpu) { rqs[cpu].owner = -1; spin_unlock(&rqs[cpu].lock); }
#define BALANCE_INTERVAL_NS 20000000ULL
static uint64_t last_balance_ns;

static void rq_init(void) {
    for (int c = 0; c < MAX_CPUS; c++) {
        rqs[c].owner = -1;
        list_init(&rqs[c].rt);
        for (int i = 0; i < RQ_LEVELS; i++) list_init(&rqs[c].q[i]);
    }
}
static bool allowed(struct thread *t, int cpu) { return (t->affinity >> cpu) & 1; }
static bool migratable(struct thread *t, int cpu) { return (t->affinity & ~(1ULL << cpu)) != 0; }
static void rq_add_locked(int cpu, struct thread *t) {
    __atomic_fetch_add(&rqs[cpu].nr, 1, __ATOMIC_RELAXED);
    if (migratable(t, cpu)) __atomic_fetch_add(&rqs[cpu].nr_mig, 1, __ATOMIC_RELAXED);
    if (is_rt(t)) {
        list_for_each(it, &rqs[cpu].rt) {
            struct thread *o = list_entry(it, struct thread, run_node);
            if (o->rt_prio < t->rt_prio) { __list_add(&t->run_node, it->prev, it); return; }
        }
        list_add_tail(&rqs[cpu].rt, &t->run_node);
        return;
    }
    list_add_tail(&rqs[cpu].q[level_of(t)], &t->run_node);
}
/* Remove before changing affinity: nr_mig must reflect the mask used at insertion. */
static void rq_remove_locked(int cpu, struct thread *t) {
    list_del(&t->run_node);
    __atomic_fetch_sub(&rqs[cpu].nr, 1, __ATOMIC_RELAXED);
    if (migratable(t, cpu)) __atomic_fetch_sub(&rqs[cpu].nr_mig, 1, __ATOMIC_RELAXED);
}
static void rq_add(int cpu, struct thread *t) { rq_lock(cpu); rq_add_locked(cpu, t); rq_unlock(cpu); }
/* highest-priority thread in rqs[from] that may run on 'cpu' */
static struct thread *rq_take_locked(int from, int cpu) {
    struct rq *r = &rqs[from];
    if (!r->nr) return nullptr;
    list_for_each(it, &r->rt) {
        struct thread *t = list_entry(it, struct thread, run_node);
        if (!allowed(t, cpu) || (t != current && __atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE))) continue;
        rq_remove_locked(from, t);
        return t;
    }
    for (int i = 0; i < RQ_LEVELS; i++) {
        /* threads in slice debt are refilled and rotated to the tail (bounded passes) */
        for (int pass = 0, n = r->nr * 4; pass < n && !list_empty(&r->q[i]); pass++) {
            struct thread *t = list_first(&r->q[i], struct thread, run_node);
            if (!allowed(t, cpu) || !policy_skip(t)) break;
            list_del(&t->run_node);
            list_add_tail(&r->q[i], &t->run_node);
        }
        list_for_each(it, &r->q[i]) {
            struct thread *t = list_entry(it, struct thread, run_node);
            if (!allowed(t, cpu) || (t != current && __atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE))) continue;
            rq_remove_locked(from, t);
            return t;
        }
    }
    return nullptr;
}
static struct thread *rq_take(int from, int cpu) {
    rq_lock(from);
    struct thread *t = rq_take_locked(from, cpu);
    rq_unlock(from);
    return t;
}

#ifdef CONFIG_SCHED_MLFQ
/* rule 5: every MLFQ_BOOST_MS move everything back to the top queue */
static void policy_tick(void) {
    if (jiffies - last_boost < MLFQ_BOOST_MS) return;
    last_boost = jiffies;
    for (int c = 0; c < ncpus; c++) rq_lock(c);
    for (int c = 0; c < ncpus; c++)
        for (int i = 1; i < MLFQ_LEVELS; i++)
            list_for_each_safe(it, tmp, &rqs[c].q[i]) { list_del(it); list_add_tail(&rqs[c].q[0], it); }
    list_for_each(it, &all_threads) {
        struct thread *t = list_entry(it, struct thread, all_node);
        if (t->level) { t->level = 0; t->quantum = quantum_for(t); }
    }
    for (int c = ncpus - 1; c >= 0; c--) rq_unlock(c);
}
#else
static void policy_tick(void) {}
#endif

/* /proc/sched thread listing (caller holds the BKL, which protects all_threads) */
void sched_for_each_thread(void (*fn)(struct thread *, void *), void *arg) {
    list_for_each(it, &all_threads) fn(list_entry(it, struct thread, all_node), arg);
}

static bool cpu_idle(struct cpu *c) { return c->online && c->cur == c->idle && !rqs[c->id].nr; }

/* where should a thread that just became runnable go? */
static int select_cpu(struct thread *t) {
    int last = t->cpu ? t->cpu->id : 0;
    if (allowed(t, last) && cpu_idle(&cpus[last])) return last;
    for (int i = 0; i < ncpus; i++)
        if (allowed(t, i) && cpu_idle(&cpus[i])) return i;
    if (allowed(t, last) && cpus[last].online) return last;
    int best = -1;
    for (int i = 0; i < ncpus; i++)
        if (allowed(t, i) && cpus[i].online && (best < 0 || rqs[i].nr < rqs[best].nr)) best = i;
    return best < 0 ? 0 : best;
}

/* ---------------------------------------------------------------- run queue */
static void enqueue_on(int cpu, struct thread *t) {
    rq_lock(cpu);
    t->state = T_RUNNABLE;
    t->rq_cpu = cpu;
    rq_add_locked(cpu, t);
    __atomic_fetch_add(&nr_runnable, 1, __ATOMIC_RELAXED);
    rq_unlock(cpu);
}
static void enqueue(struct thread *t) { enqueue_on(select_cpu(t), t); }

/* next thread for CPU c: its own queue, else steal from the busiest queue */
static struct thread *dequeue(struct cpu *c) {
    struct thread *t = rq_take(c->id, c->id);
    if (!t) {
        uint64_t tried = 1ULL << c->id;
        /* A busy queue may contain only pinned threads. Try the next donor rather than
         * leaving this CPU idle when a smaller queue has eligible work. */
        for (int pass = 0; pass < ncpus - 1; pass++) {
            int victim = -1;
            for (int i = 0; i < ncpus; i++)
                if (!(tried & (1ULL << i)) && cpus[i].online && rqs[i].nr_mig &&
                    (victim < 0 || rqs[i].nr > rqs[victim].nr)) victim = i;
            if (victim < 0) break;
            tried |= 1ULL << victim;
            if ((t = rq_take(victim, c->id))) { rqs[c->id].steals++; break; }
        }
    }
    if (t) __atomic_fetch_sub(&nr_runnable, 1, __ATOMIC_RELAXED);
    return t;
}

int sched_runnable_count(void) { return __atomic_load_n(&nr_runnable, __ATOMIC_RELAXED); }

/* could CPU c find something to run? (lock-free, approximate; dequeue() decides for real) */
static bool cpu_has_work(struct cpu *c) {
    if (__atomic_load_n(&rqs[c->id].nr, __ATOMIC_RELAXED)) return true;
    for (int i = 0; i < ncpus; i++)
        if (i != c->id && __atomic_load_n(&rqs[i].nr_mig, __ATOMIC_RELAXED)) return true;
    return false;
}

static void kick_after_wake(struct thread *t);
static uint64_t online_mask(void) {
    uint64_t m = 0;
    for (int i = 0; i < ncpus && i < 64; i++) if (cpus[i].online) m |= 1ULL << i;
    return m;
}

/* cpu0 tick, sched_lock held. Move at most one normal thread per destination per
 * interval, including to busy CPUs. Idle stealing alone cannot balance busy queues.
 * Leave RT placement to wake-up/stealing, preserve affinity, and never move a context
 * whose switch-out is still live. Search low-priority tails to preserve local latency. */
static int rq_load(int cpu) {
    struct cpu *c = &cpus[cpu];
    return rqs[cpu].nr + (c->cur && c->cur != c->idle);
}
static struct thread *balance_candidate(int from, int to) {
    for (int i = RQ_LEVELS - 1; i >= 0; i--) {
        struct list_node *head = &rqs[from].q[i];
        for (struct list_node *it = head->prev; it != head; it = it->prev) {
            struct thread *t = list_entry(it, struct thread, run_node);
            if (allowed(t, to) && !__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) return t;
        }
    }
    return nullptr;
}
static void balance_tick(uint64_t now) {
    if (now - last_balance_ns < BALANCE_INTERVAL_NS) return;
    last_balance_ns = now;
    for (int to = 0; to < ncpus; to++) {
        if (!cpus[to].online) continue;
        uint64_t tried = 1ULL << to;
        for (int pass = 0; pass < ncpus - 1; pass++) {
            int from = -1;
            for (int i = 0; i < ncpus; i++)
                if (!(tried & (1ULL << i)) && cpus[i].online && rqs[i].nr_mig &&
                    (from < 0 || rq_load(i) > rq_load(from))) from = i;
            if (from < 0) break;
            tried |= 1ULL << from;
            int lo = MIN(from, to), hi = MAX(from, to);
            rq_lock(lo); rq_lock(hi);
            struct thread *chosen = rq_load(from) > rq_load(to) + 1 ? balance_candidate(from, to) : nullptr;
            if (chosen) {
                rq_remove_locked(from, chosen);
                chosen->rq_cpu = to;
                rq_add_locked(to, chosen);
                rqs[to].balances++;
            }
            rq_unlock(hi); rq_unlock(lo);
            if (chosen) { kick_after_wake(chosen); break; }
        }
    }
}

/* wait/state lock held. Local switches may change RUNNING/RUNNABLE concurrently,
 * so resolve and revalidate the protecting rq before reading/modifying a target. */
static int target_rq_lock(struct thread *t) {
    for (;;) {
        int cpu = __atomic_load_n(&t->rq_cpu, __ATOMIC_ACQUIRE);
        rq_lock(cpu);
        if (cpu == __atomic_load_n(&t->rq_cpu, __ATOMIC_ACQUIRE)) return cpu;
        rq_unlock(cpu);
    }
}
/* change policy/priority; requeues the thread if it is waiting on a run queue */
int sched_set_policy(struct thread *t, int policy, int rt_prio, int nice) {
    if (policy == SCHED_FIFO_ || policy == SCHED_RR_) { if (rt_prio < 1 || rt_prio > 99) return -EINVAL; }
    else if (policy == 0 || policy == 3 || policy == 5) { if (rt_prio != 0) return -EINVAL; }     /* OTHER, BATCH, IDLE */
    else return -EINVAL;
    if (nice < -20) nice = -20;
    if (nice > 19) nice = 19;
    uint64_t f = sl_lock_irqsave();
    int cpu = target_rq_lock(t);
    bool queued = t->state == T_RUNNABLE && t != this_cpu()->idle;
    if (queued) rq_remove_locked(cpu, t);
    t->policy = policy; t->rt_prio = rt_prio; t->nice = nice;
    t->quantum = quantum_for(t);
    if (queued) rq_add_locked(cpu, t);
    else if (t->state == T_RUNNING && t->cpu) {      /* may now be preemptable: re-evaluate */
        if (t->cpu == this_cpu()) this_cpu()->resched = true;
        else smp_send_resched(t->cpu);
    }
    rq_unlock(cpu);
    if (queued) kick_after_wake(t);
    sl_unlock_irqrestore(f);
    return 0;
}

int sched_set_affinity(struct thread *t, uint64_t mask) {
    mask &= online_mask();
    if (!mask) return -EINVAL;
    uint64_t f = sl_lock_irqsave();
    int cpu = target_rq_lock(t);
    bool queued = t->state == T_RUNNABLE && t != t->cpu->idle;
    int old_cpu = t->rq_cpu;
    if (queued) rq_remove_locked(old_cpu, t);
    t->affinity = mask;
    rq_unlock(cpu);
    if (queued) {
        /* Even if the queue does not change, narrowing/widening updates nr_mig. */
        t->rq_cpu = allowed(t, old_cpu) ? old_cpu : select_cpu(t);
        rq_add(t->rq_cpu, t);
        kick_after_wake(t);
    } else if (t->state == T_RUNNING && t->cpu && !allowed(t, t->cpu->id)) {   /* migrate at next schedule() */
        if (t->cpu == this_cpu()) this_cpu()->resched = true;
        else smp_send_resched(t->cpu);
    }
    sl_unlock_irqrestore(f);
    return 0;
}
int sched_rq_len(int cpu) { return rqs[cpu].nr; }
uint64_t sched_rq_steals(int cpu) { return rqs[cpu].steals; }
uint64_t sched_rq_balances(int cpu) { return rqs[cpu].balances; }
uint64_t sched_rq_local(int cpu) { return rqs[cpu].local_schedules; }
uint64_t sched_rq_coordinated(int cpu) { return rqs[cpu].coordinated_schedules; }

/* A thread was queued on t->rq_cpu: get that CPU to look at it (IPI or local resched). */
static void kick_after_wake(struct thread *t) {
    int cpu = target_rq_lock(t);
    struct cpu *self = this_cpu(), *c = &cpus[cpu];
    if (t->state != T_RUNNABLE) { rq_unlock(cpu); return; }
    if (c == self) {
        if (self->cur == self->idle || !self->cur || wake_preempts(t, self->cur)) self->resched = true;
        rq_unlock(c->id); return;
    }
    if (c->cur == c->idle || !c->cur || wake_preempts(t, c->cur))
        if (!(c->ipi_pending & IPI_RESCHED)) smp_send_resched(c);
    rq_unlock(c->id);
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
    t->bkl_depth = 0;
    t->bkl_saved = 1;        /* new threads take the BKL in sched_finish_switch() before running */
    t->cpu = this_cpu();
    t->affinity = current ? current->affinity : ~0ULL;
    if (current) { t->nice = current->nice; t->policy = current->policy; t->rt_prio = current->rt_prio; }
    t->quantum = quantum_for(t);
    if (!t->affinity) t->affinity = ~0ULL;
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
    struct list_node dead = LIST_INIT(dead);
    sl_lock();
    list_for_each_safe(it, tmp, &zombies) {
        struct thread *t = list_entry(it, struct thread, run_node);
        if (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&t->handoff_refs, __ATOMIC_ACQUIRE)) continue;
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

/* Own rq is held across the machine switch, never the wait/state coordinator.
 * A reference pins the old context after on_cpu is cleared: it can immediately
 * run/switch on another CPU while we inspect a wake that raced with switch-out. */
static void idle_timer_exit(struct cpu *c);
static void switch_locked(struct cpu *c, struct thread *prev, struct thread *next, bool coordinated) {
    assert(rqs[c->id].owner == c->id);
    next->state = T_RUNNING;
    next->rq_cpu = c->id;
    pick_reset_quantum(next);
    if (next == prev) {
        if (coordinated) sl_unlock();
        sched_finish_switch(); return;
    }
    bkl_drop_for_switch(prev);      /* only a real stack switch drops the BKL */
    if (prev == c->idle) idle_timer_exit(c);
    assert(!__atomic_load_n(&next->on_cpu, __ATOMIC_ACQUIRE));
    __atomic_fetch_add(&prev->handoff_refs, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&next->on_cpu, 1, __ATOMIC_RELEASE);
    next->cpu = c;
    __atomic_store_n(&c->cur, next, __ATOMIC_RELEASE);
    c->ctx_switches++;
    uint64_t now = time_ns();
    if (prev != c->idle) {
        uint64_t d = now - prev->exec_start_ns;
        prev->sum_exec_ns += d;
        bool invol = prev->state == T_RUNNING || prev->state == T_RUNNABLE;
        if (invol) prev->nivcsw++; else prev->nvcsw++;
        if (prev->proc) {
            __atomic_fetch_add(&prev->proc->sum_exec_ns, d, __ATOMIC_RELAXED);
            __atomic_fetch_add(invol ? &prev->proc->nivcsw : &prev->proc->nvcsw, 1, __ATOMIC_RELAXED);
        }
    }
    next->exec_start_ns = now;
    c->prev = prev;
    if (coordinated) sl_unlock();
    arch_set_current(next);
    arch_switch_to(prev, next);
    sched_finish_switch();
}

/* Blocking, affinity migration and cross-CPU steals coordinate wait/state
 * transitions briefly; the coordinator is released before switching stacks. */
[[gnu::noinline]] static void __schedule(void) {
    struct cpu *c = this_cpu();
    struct thread *prev = c->cur, *next;
    if (sched_owner != c->id || prev != current)
        panic("__schedule: cpu%d sched_lock owner %d prev %s current %s from %p", c->id, sched_owner, prev->name,
              current->name, __builtin_return_address(0));
    c->resched = false;
    if (prev->state == T_RUNNING && prev != c->idle) {
        if (allowed(prev, c->id)) enqueue_on(c->id, prev);   /* round robin stays local */
        else { enqueue(prev); kick_after_wake(prev); }         /* affinity changed */
    }
    next = dequeue(c);
    if (!next) next = c->idle;
    rq_lock(c->id);
    rqs[c->id].coordinated_schedules++;
    switch_locked(c, prev, next, true);
}

/* CPU-bound/yield/preemption hot path: only this CPU's rq lock. A foreign
 * context still saving its registers is skipped rather than spun on. */
static bool schedule_local(void) {
    struct cpu *c = this_cpu();
    struct thread *prev = current;
    rq_lock(c->id);
    if (prev->state != T_RUNNING || (prev != c->idle && !allowed(prev, c->id)) ||
        (!rqs[c->id].nr && cpu_has_work(c))) {
        rq_unlock(c->id); return false;
    }
    c->resched = false;
    if (prev != c->idle) {
        prev->state = T_RUNNABLE; prev->rq_cpu = c->id;
        rq_add_locked(c->id, prev);
        __atomic_fetch_add(&nr_runnable, 1, __ATOMIC_RELAXED);
    }
    struct thread *next = rq_take_locked(c->id, c->id);
    if (next) __atomic_fetch_sub(&nr_runnable, 1, __ATOMIC_RELAXED);
    else next = c->idle;
    rqs[c->id].local_schedules++;
    switch_locked(c, prev, next, false);
    return true;
}
/* runs on the new thread right after a switch: the previous thread's context is now saved */
void sched_finish_switch(void) {
    struct cpu *c = this_cpu();
    assert(rqs[c->id].owner == c->id && sched_owner != c->id);
    struct thread *old = c->prev;
    if (old) {
        __atomic_store_n(&old->on_cpu, 0, __ATOMIC_RELEASE);
        c->prev = nullptr;
    }
    rq_unlock(c->id);
    if (old) {
        if (__atomic_load_n(&old->state, __ATOMIC_ACQUIRE) == T_RUNNABLE &&
            __atomic_load_n(&old->rq_cpu, __ATOMIC_ACQUIRE) != c->id)
            kick_after_wake(old);
        __atomic_fetch_sub(&old->handoff_refs, 1, __ATOMIC_RELEASE);
    }
    bkl_retake_after_switch(current);
}

void schedule(void) {
    uint64_t f = arch_irq_save();
    if (!schedule_local()) { sl_lock(); __schedule(); }
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
/*
 * Charge the time since the thread's last accounting point (switch-in or previous tick) as
 * user or system time, as sampled by this tick. Ticks can be lost (e.g. an emulator vCPU
 * descheduled by the host), so the elapsed time is used rather than "one tick". Returns the
 * elapsed milliseconds (>= 1) for time-slice accounting.
 */
static int account_tick(struct cpu *c, struct thread *cur) {
    uint64_t now = time_ns(), last = cur->acct_ns > cur->exec_start_ns ? cur->acct_ns : cur->exec_start_ns;
    uint64_t d = now > last ? now - last : 0;
    cur->acct_ns = now;
    cur->run_ticks++;
    if (c->tick_user) { cur->utime_ns += d; c->user_ticks++; }
    else { cur->stime_ns += d; c->sys_ticks++; }
    if (cur->proc) __atomic_fetch_add(c->tick_user ? &cur->proc->utime_ns : &cur->proc->stime_ns, d, __ATOMIC_RELAXED);
    int ms = (int)((d + 500000) / 1000000);
    return ms < 1 ? 1 : ms > 1000 ? 1000 : ms;
}

bool sched_tick_fast(bool from_user) {
    struct cpu *c = this_cpu();
    c->tick_user = from_user;
    if (c->id == 0 || !c->cur) return false;
    c->ticks++;
    c->tick_accounted = true;
    struct thread *cur = c->cur;
    if (cur == c->idle) {
        if (!c->tick_stopped) c->idle_ticks++;
        if (cpu_has_work(c)) { c->resched = true; return false; }
        c->tick_accounted = false;
        return true;
    }
    rq_lock(c->id);
    cur->quantum -= account_tick(c, cur);
    if (cur->quantum <= 0) { quantum_expired(cur); c->resched = true; }
    bool slow = c->resched || (from_user && (signal_pending(cur) || (cur->proc && cur->proc->alarm_ns)));
    rq_unlock(c->id);
    if (slow) return false;
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
        balance_tick(now);
        sl_unlock();
    }
    if (c->tick_accounted) { c->tick_accounted = false; return; }
    c->ticks++;
    struct thread *cur = c->cur;
    if (!cur) return;
    if (cur == c->idle) {
        if (!c->tick_stopped) c->idle_ticks++;
        if (cpu_has_work(c)) c->resched = true;
        return;
    }
    rq_lock(c->id);
    cur->quantum -= account_tick(c, cur);
    if (cur->quantum <= 0) {
        quantum_expired(cur);
        c->resched = true;
    }
    rq_unlock(c->id);
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
    sched_timer_changed();
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
    sched_timer_changed();
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

/* IRQs are disabled while arming/rechecking the timer. CPU0 owns global
 * deadlines; APs can disable their timer entirely and rely on wakeup IPIs. */
void sched_timer_changed(void) {
    if (this_cpu()->id != 0 && cpus[0].online &&
        __atomic_load_n(&cpus[0].cur, __ATOMIC_ACQUIRE) == cpus[0].idle)
        smp_send_resched(&cpus[0]);
}
uint64_t timerfd_next_deadline(void);
static uint64_t idle_deadline(struct cpu *c, uint64_t now) {
    if (c->id != 0) return UINT64_MAX;
    sl_lock();
    uint64_t deadline = last_balance_ns + BALANCE_INTERVAL_NS;
    list_for_each(it, &sleep_list) {
        struct thread *t = list_entry(it, struct thread, timer_node);
        if (t->wake_ns < deadline) deadline = t->wake_ns;
    }
#ifdef CONFIG_SCHED_MLFQ
    uint64_t boost = (last_boost + MLFQ_BOOST_MS) * 1000000ULL;
    if (boost < deadline) deadline = boost;
#endif
    sl_unlock();
    uint64_t fd = timerfd_next_deadline();
    if (fd < deadline) deadline = fd;
    uint64_t poll = arch_idle_poll_ns();
    if (poll != UINT64_MAX && now + poll < deadline) deadline = now + poll;
    return deadline > now ? deadline : now + 1000;
}
static void idle_timer_exit(struct cpu *c) {
    if (!c->tick_stopped) return;
    __atomic_add_fetch(&c->idle_seq, 1, __ATOMIC_ACQ_REL);
    uint64_t now = time_ns();
    uint64_t elapsed = now - c->idle_start_ns + c->idle_remainder_ns;
    c->idle_ticks += elapsed / 1000000;
    c->idle_remainder_ns = elapsed % 1000000;
    c->tick_stopped = false;
    __atomic_add_fetch(&c->idle_seq, 1, __ATOMIC_RELEASE);
    arch_timer_active();
}
/* Include the currently halted interval: reading /proc must not need an IPI
 * just to make a sleeping CPU's idle counter advance. Writers are IRQ-off. */
uint64_t sched_idle_ticks(int cpu) {
    struct cpu *c = &cpus[cpu];
    for (;;) {
        uint32_t seq = __atomic_load_n(&c->idle_seq, __ATOMIC_ACQUIRE);
        if (seq & 1) { arch_cpu_relax(); continue; }
        uint64_t ticks = __atomic_load_n(&c->idle_ticks, __ATOMIC_RELAXED);
        bool stopped = __atomic_load_n(&c->tick_stopped, __ATOMIC_RELAXED);
        uint64_t since = __atomic_load_n(&c->idle_start_ns, __ATOMIC_RELAXED);
        uint64_t remainder = __atomic_load_n(&c->idle_remainder_ns, __ATOMIC_RELAXED);
        uint64_t now = time_ns();
        if (seq != __atomic_load_n(&c->idle_seq, __ATOMIC_ACQUIRE)) continue;
        if (stopped && now >= since) ticks += (now - since + remainder) / 1000000;
        return ticks;
    }
}
/* The remote-work hint used for stealing can be a false positive (a donor
 * may only allow other CPUs). Do not let that hint prevent WFI forever. New
 * local placement or a deadline change sets resched/IPI; CPU0 also balances. */
static bool idle_wake_pending(struct cpu *c) {
    return __atomic_load_n(&rqs[c->id].nr, __ATOMIC_RELAXED) || c->resched ||
           __atomic_load_n(&c->ipi_pending, __ATOMIC_ACQUIRE);
}
/* Idle threads run without the BKL; IRQ handlers acquire it as needed. */
static void idle_loop(void *arg) {
    struct cpu *c = this_cpu();
    for (;;) {
        arch_irq_disable();
        if (__atomic_load_n(&c->ipi_pending, __ATOMIC_ACQUIRE)) ipi_handle();
        c->resched = false;
        reap_zombies();
        if (cpu_has_work(c)) {
            uint64_t sw = c->ctx_switches;
            sl_lock();
            __schedule();
            if (c->ctx_switches != sw) { arch_irq_enable(); continue; }
        }
        uint64_t now = time_ns();
        uint64_t deadline = idle_deadline(c, now);
        __atomic_add_fetch(&c->idle_seq, 1, __ATOMIC_ACQ_REL);
        c->idle_start_ns = time_ns();
        c->tick_stopped = true;
        c->idle_sleeps++;
        __atomic_add_fetch(&c->idle_seq, 1, __ATOMIC_RELEASE);
        arch_timer_idle(deadline);
        /* A remote enqueue after this check necessarily sends an interrupt.
         * Architecture WFI helpers enable IRQs atomically with entering idle. */
        if (!idle_wake_pending(c))
            arch_wait_for_interrupt();
        arch_irq_disable();             /* WFI helpers return with IRQs enabled */
        idle_timer_exit(c);
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
    boot->affinity = ~0ULL;
    c->online = true;
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
    rq_lock(c->id);             /* released by the idle thread in sched_finish_switch() */
    c->online = true;
    c->cur = c->idle;
    c->prev = d;
    d->handoff_refs = 1;
    c->idle->on_cpu = 1;
    c->idle->state = T_RUNNING;
    arch_set_current(c->idle);
    arch_switch_to(d, c->idle);
    panic("AP boot context resumed");
}

/* A user syscall that just waited for the BKL must get to finish its short
 * critical section before another kernel-mode IRQ preempts it. Otherwise many
 * vCPUs can endlessly acquire -> preempt -> drop -> retake the ticket lock and
 * never execute the operation. Explicit waits/yields still drop the BKL; user
 * return and BKL-free/kernel-thread contexts remain preemptible. */
bool sched_kernel_preemptible(void) {
    return !current || !current->proc || current->bkl_depth == 0;
}
void trap_exit_hook_sched(void) {
    if (need_resched && current && sched_kernel_preemptible()) schedule();
}
