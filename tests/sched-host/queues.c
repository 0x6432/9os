/* Include the real implementation to test its private queue helpers. Unused kernel
 * paths are link-time garbage-collected; only IRQ/current/IPI hardware is mocked. */
/* Do not override libc's sched_yield, which the sanitizer runtime itself uses. */
#define sched_yield kernel_sched_yield
#include "../../kernel/core/sched.c"
#undef sched_yield
#include <pthread.h>

extern int printf(const char *, ...);
extern void abort(void);
struct cpu cpus[MAX_CPUS];
int ncpus;
volatile uint64_t jiffies;
_Thread_local struct thread *test_current;
_Thread_local int sched_test_cpu;
static struct thread idle[MAX_CPUS], running[MAX_CPUS], threads[16];
static int checks;
#define CHECK(c) do { __atomic_fetch_add(&checks, 1, __ATOMIC_RELAXED); if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); abort(); } } while (0)
void smp_send_resched(struct cpu *c) { c->resched = true; c->ipi_pending |= IPI_RESCHED; }
void lockdep_acquire(const struct lock_class *c, bool sleeping) { (void)c; (void)sleeping; }
void lockdep_release(const struct lock_class *c, bool sleeping) { (void)c; (void)sleeping; }
void spin_lock_ipi(spinlock_t *l) { spin_lock(l); }

static uint64_t fake_now, fake_fd = UINT64_MAX, fake_poll = UINT64_MAX;
static int machine_switches, timer_restarts, bkl_drops;
uint64_t time_ns(void) { return fake_now; }
uint64_t timerfd_next_deadline(void) { return fake_fd; }
uint64_t arch_idle_poll_ns(void) { return fake_poll; }
void arch_timer_active(void) { timer_restarts++; }
void bkl_drop_for_switch(struct thread *t) {
    if (t->bkl_depth) { t->bkl_saved = t->bkl_depth; t->bkl_depth = 0; __atomic_fetch_add(&bkl_drops, 1, __ATOMIC_RELAXED); }
}
void uaccess_restore_after_switch(void) {}
void bkl_retake_after_switch(struct thread *t) {
    CHECK(!rqs[this_cpu()->id].lock.locked && sched_owner != this_cpu()->id);
    if (t->bkl_saved) { t->bkl_depth = t->bkl_saved; t->bkl_saved = 0; }
}
void arch_switch_to(struct thread *prev, struct thread *next) {
    CHECK(rqs[this_cpu()->id].lock.locked && sched_owner != this_cpu()->id);
    CHECK(prev->handoff_refs == 1 && prev->on_cpu && next->on_cpu);
    __atomic_fetch_add(&machine_switches, 1, __ATOMIC_RELAXED);
}
void panic(const char *fmt, ...) { printf("kernel assertion: %s\n", fmt); abort(); }

static void reset(int count) {
    sched_test_cpu = 0;
    memset(cpus, 0, sizeof cpus);
    memset(rqs, 0, sizeof rqs);
    memset(idle, 0, sizeof idle);
    memset(running, 0, sizeof running);
    memset(threads, 0, sizeof threads);
    rq_init();
    list_init(&all_threads);
    nr_runnable = 0;
    last_balance_ns = 0;
    sched_owner = -1;
    fake_now = 100000000;
    fake_fd = fake_poll = UINT64_MAX;
    machine_switches = timer_restarts = bkl_drops = 0;
    list_init(&sleep_list);
#ifdef CONFIG_SCHED_MLFQ
    last_boost = jiffies = 0;
#endif
    ncpus = count;
    for (int i = 0; i < count; i++) {
        cpus[i].id = i;
        cpus[i].online = true;
        cpus[i].idle = &idle[i];
        cpus[i].cur = &running[i];   /* every CPU is busy */
        running[i].cpu = &cpus[i];
        running[i].state = T_RUNNING;
        running[i].affinity = (1ULL << count) - 1;
        running[i].on_cpu = 1;
        running[i].rq_cpu = i;
        running[i].quantum = 7;
        idle[i].cpu = &cpus[i];
        idle[i].affinity = 1ULL << i;
        idle[i].rq_cpu = i;
    }
    test_current = &running[0];
}
static struct thread *add(int id, int cpu, uint64_t mask, int policy) {
    struct thread *t = &threads[id];
    t->cpu = &cpus[cpu];
    t->affinity = mask;
    t->policy = policy;
    t->rt_prio = policy ? 10 : 0;
    t->quantum = 7;
    enqueue_on(cpu, t);
    return t;
}
static void invariants(void) {
    int total = 0;
    for (int c = 0; c < ncpus; c++) {
        int nr = 0, mig = 0;
        for (int q = -1; q < RQ_LEVELS; q++) {
            struct list_node *head = q < 0 ? &rqs[c].rt : &rqs[c].q[q];
            list_for_each(it, head) {
                struct thread *t = list_entry(it, struct thread, run_node);
                CHECK(t->state == T_RUNNABLE && t->rq_cpu == c && allowed(t, c));
                nr++;
                if (migratable(t, c)) mig++;
            }
        }
        CHECK(nr == rqs[c].nr && mig == rqs[c].nr_mig);
        total += nr;
    }
    CHECK(total == nr_runnable);
}
static void affinity_test(void) {
    reset(2);
    struct thread *t = add(0, 0, 3, 0);
    CHECK(rqs[0].nr_mig == 1);
    CHECK(sched_set_affinity(t, 1) == 0 && rqs[0].nr_mig == 0);
    CHECK(sched_set_affinity(t, 3) == 0 && rqs[0].nr_mig == 1);
    CHECK(sched_set_affinity(t, 2) == 0);
    CHECK(rqs[0].nr == 0 && rqs[0].nr_mig == 0 && rqs[1].nr == 1 && rqs[1].nr_mig == 0);
    CHECK(sched_set_affinity(t, 0) == -EINVAL && t->affinity == 2);
    CHECK(sched_set_affinity(t, 4) == -EINVAL && t->affinity == 2);
    CHECK(sched_set_policy(t, SCHED_FIFO_, 20, 0) == 0);
    CHECK(sched_set_policy(t, 0, 0, 0) == 0);
    CHECK(sched_set_policy(t, SCHED_FIFO_, 0, 0) == -EINVAL);
    CHECK(sched_set_policy(t, SCHED_RR_, 100, 0) == -EINVAL);
    CHECK(sched_set_policy(t, 0, 1, 0) == -EINVAL);
    CHECK(sched_set_policy(t, 99, 0, 0) == -EINVAL);
    CHECK(t->policy == 0 && t->rt_prio == 0);
    invariants();
}
static void steal_test(void) {
    reset(4);
    for (int i = 0; i < 3; i++) add(i, 1, (1ULL << 1) | (1ULL << 3), 0);
    struct thread *eligible = add(3, 2, (1ULL << 0) | (1ULL << 2), 0);
    /* Biggest donor allows migration, but not to CPU0. Smaller donor must be tried. */
    CHECK(dequeue(&cpus[0]) == eligible);
    CHECK(rqs[0].steals == 1 && nr_runnable == 3);
    CHECK(dequeue(&cpus[0]) == nullptr);
    invariants();
}
static void balance_test(void) {
    reset(2);
    for (int i = 0; i < 6; i++) add(i, 0, 3, 0);
    threads[5].on_cpu = 1;
    balance_tick(BALANCE_INTERVAL_NS - 1);
    CHECK(rqs[1].nr == 0);
    balance_tick(BALANCE_INTERVAL_NS);
    CHECK(rqs[1].nr == 1 && rqs[1].balances == 1 && nr_runnable == 6);
    CHECK(threads[5].rq_cpu == 0);         /* context still live: cannot migrate */
    CHECK(threads[4].rq_cpu == 1 && threads[4].quantum == 7);
    invariants();
    reset(2);
    for (int i = 0; i < 6; i++) add(i, 0, 1, 0);
    balance_tick(BALANCE_INTERVAL_NS);
    CHECK(rqs[1].nr == 0);                /* pinned threads stay pinned */
    invariants();
    reset(2);
    for (int i = 0; i < 6; i++) add(i, 0, 3, SCHED_FIFO_);
    balance_tick(BALANCE_INTERVAL_NS);
    CHECK(rqs[1].nr == 0);                /* RT placement is not changed by balancing */
    invariants();
    reset(2);
    for (int i = 0; i < 6; i++) add(i, 0, 3, 0);
    cpus[1].online = false;
    balance_tick(BALANCE_INTERVAL_NS);
    CHECK(rqs[1].nr == 0);                /* never target an offline CPU */
    invariants();
}
static void churn_test(void) {
    reset(4);
    for (int i = 0; i < 16; i++) add(i, i % 4, 15, 0);
    for (int step = 1; step <= 200; step++) {
        struct thread *t = &threads[step % 16];
        uint64_t mask = step % 2 ? 1ULL << (step % 4) : 15;
        CHECK(sched_set_affinity(t, mask) == 0);
        balance_tick(step * BALANCE_INTERVAL_NS);
        invariants();
    }
}
static void local_switch_test(void) {
    reset(2);
    running[0].bkl_depth = 1;
    CHECK(schedule_local());              /* self-selection does not switch stacks */
    CHECK(!bkl_drops && running[0].bkl_depth == 1 && !running[0].bkl_saved);
    CHECK(machine_switches == 0 && cpus[0].cur == &running[0]);
    CHECK(rqs[0].local_schedules == 1 && !rqs[0].coordinated_schedules);
    struct thread *t = add(0, 0, 3, 0);
    CHECK(schedule_local());
    CHECK(cpus[0].cur == t && machine_switches == 1 && t->on_cpu);
    CHECK(bkl_drops == 1 && running[0].bkl_saved == 1 && !running[0].bkl_depth);
    CHECK(!running[0].on_cpu && !running[0].handoff_refs && nr_runnable == 1);
    invariants();
    reset(2);
    t = add(0, 0, 3, SCHED_FIFO_); t->on_cpu = 1;
    CHECK(schedule_local() && cpus[0].cur == &running[0]); /* live foreign context skipped */
    CHECK(!machine_switches && nr_runnable == 1);
    t->on_cpu = 0;
    CHECK(schedule_local() && cpus[0].cur == t);
    CHECK(machine_switches == 1 && !t->handoff_refs);
    reset(2);
    running[0].affinity = 2;
    CHECK(!schedule_local() && !machine_switches); /* migration uses coordinator */
    reset(2);
    add(0, 1, 3, 0);
    CHECK(!schedule_local() && !machine_switches); /* remote stealing uses coordinator */
}
static void handoff_wake_test(void) {
    reset(2);
    struct thread *old = add(0, 1, 3, 0);
    old->on_cpu = 1; old->handoff_refs = 1;
    cpus[0].prev = old;
    cpus[1].cur = cpus[1].idle;
    rq_lock(0); sched_finish_switch();
    CHECK(!old->on_cpu && !old->handoff_refs && !cpus[0].prev);
    CHECK(cpus[1].resched && (cpus[1].ipi_pending & IPI_RESCHED));
    CHECK(!rqs[0].lock.locked && nr_runnable == 1);
    reset(2);
    running[0].policy = SCHED_FIFO_; running[0].rt_prio = 20;
    struct thread *equal = add(0, 0, 3, SCHED_FIFO_); equal->rt_prio = 20;
    kick_after_wake(equal); CHECK(!cpus[0].resched); /* equal FIFO may not preempt */
    equal->rt_prio = 21; kick_after_wake(equal); CHECK(cpus[0].resched);
    cpus[0].resched = false; equal->policy = 0;
    kick_after_wake(equal); CHECK(!cpus[0].resched); /* normal may not preempt RT */
}
static void deadline_test(void) {
    reset(3);
    add(0, 1, 6, 0);                    /* migratable, but never eligible for CPU0 */
    CHECK(cpu_has_work(&cpus[0]) && !idle_wake_pending(&cpus[0]));
    cpus[0].resched = true; CHECK(idle_wake_pending(&cpus[0]));
    cpus[0].resched = false; cpus[0].ipi_pending = IPI_RESCHED;
    CHECK(idle_wake_pending(&cpus[0]));
    cpus[0].ipi_pending = 0; add(1, 0, 1, 0); CHECK(idle_wake_pending(&cpus[0]));
    reset(2);
    CHECK(idle_deadline(&cpus[1], fake_now) == UINT64_MAX);
    last_balance_ns = fake_now;
    CHECK(idle_deadline(&cpus[0], fake_now) == fake_now + BALANCE_INTERVAL_NS);
    fake_fd = fake_now + 3000000;
    CHECK(idle_deadline(&cpus[0], fake_now) == fake_fd);
    threads[0].wake_ns = fake_now + 1000000;
    list_add_tail(&sleep_list, &threads[0].timer_node);
    CHECK(idle_deadline(&cpus[0], fake_now) == threads[0].wake_ns);
    list_del(&threads[0].timer_node);
    fake_fd = UINT64_MAX; fake_poll = 2000000;
    CHECK(idle_deadline(&cpus[0], fake_now) == fake_now + fake_poll);
    fake_poll = UINT64_MAX; fake_fd = fake_now - 1;
    CHECK(idle_deadline(&cpus[0], fake_now) == fake_now + 1000);
    cpus[0].tick_stopped = true;
    cpus[0].idle_start_ns = fake_now - 2500000;
    CHECK(sched_idle_ticks(0) == 2 && cpus[0].idle_ticks == 0);
    idle_timer_exit(&cpus[0]);
    CHECK(sched_idle_ticks(0) == 2);
    CHECK(cpus[0].idle_ticks == 2 && cpus[0].idle_remainder_ns == 500000 && timer_restarts == 1);
    idle_timer_exit(&cpus[0]); CHECK(timer_restarts == 1);
    cpus[0].tick_stopped = true; cpus[0].idle_start_ns = fake_now - 500000;
    idle_timer_exit(&cpus[0]);
    CHECK(cpus[0].idle_ticks == 3 && !cpus[0].idle_remainder_ns);
    reset(2);
    cpus[0].cur = cpus[0].idle; test_current = cpus[0].idle;
    cpus[0].tick_stopped = true; cpus[0].idle_start_ns = fake_now - 7500000;
    test_current->state = T_RUNNING; test_current->on_cpu = 1;
    add(0, 0, 1, 0);
    CHECK(schedule_local());
    CHECK(!cpus[0].tick_stopped && cpus[0].idle_ticks == 7 && timer_restarts == 1);
}
static void *local_worker(void *arg) {
    sched_test_cpu = (int)(intptr_t)arg;
    test_current = cpus[sched_test_cpu].cur;
    for (int i = 0; i < 2000; i++) CHECK(schedule_local());
    return 0;
}
static void independent_locks_test(void) {
    reset(2);
    add(0, 0, 1, 0); add(1, 1, 2, 0);
    /* Holding the coordinator on CPU0 must not stall CPU1's local switch path. */
    sl_lock();
    pthread_t remote; CHECK(pthread_create(&remote, 0, local_worker, (void *)1) == 0);
    CHECK(pthread_join(remote, 0) == 0);
    CHECK(rqs[1].local_schedules == 2000 && !rqs[1].coordinated_schedules);
    sl_unlock();
    pthread_t a, b;
    CHECK(pthread_create(&a, 0, local_worker, (void *)0) == 0);
    CHECK(pthread_create(&b, 0, local_worker, (void *)1) == 0);
    CHECK(pthread_join(a, 0) == 0 && pthread_join(b, 0) == 0);
    CHECK(rqs[0].local_schedules == 2000 && rqs[1].local_schedules == 4000);
    CHECK(machine_switches == 6000 && !rqs[0].lock.locked && !rqs[1].lock.locked);
    invariants();
}
static void kernel_progress_test(void) {
    reset(2);
    static struct process process;
    running[0].proc = &process; running[0].bkl_depth = 1;
    CHECK(!sched_kernel_preemptible());
    running[0].bkl_depth = 2; CHECK(!sched_kernel_preemptible());
    running[0].bkl_depth = 0; CHECK(sched_kernel_preemptible());
    running[0].bkl_depth = 1; running[0].proc = nullptr;
    CHECK(sched_kernel_preemptible()); /* preserve preemptive kernel-thread tests */
    test_current = cpus[0].idle; CHECK(sched_kernel_preemptible());
}
int main(void) {
    affinity_test(); steal_test(); balance_test(); churn_test();
    local_switch_test(); handoff_wake_test(); deadline_test(); independent_locks_test(); kernel_progress_test();
    printf("sched-host: %d checks passed (%s)\n", checks, sched_policy_name());
    return 0;
}
