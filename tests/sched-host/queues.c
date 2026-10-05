/* Include the real implementation to test its private queue helpers. Unused kernel
 * paths are link-time garbage-collected; only IRQ/current/IPI hardware is mocked. */
/* Do not override libc's sched_yield, which the sanitizer runtime itself uses. */
#define sched_yield kernel_sched_yield
#include "../../kernel/core/sched.c"

extern int printf(const char *, ...);
extern void abort(void);
struct cpu cpus[MAX_CPUS];
int ncpus;
volatile uint64_t jiffies;
struct thread *test_current;
static struct thread idle[MAX_CPUS], running[MAX_CPUS], threads[16];
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); abort(); } } while (0)
void smp_send_resched(struct cpu *c) { c->resched = true; c->ipi_pending |= IPI_RESCHED; }

static void reset(int count) {
    memset(cpus, 0, sizeof cpus);
    memset(rqs, 0, sizeof rqs);
    memset(idle, 0, sizeof idle);
    memset(running, 0, sizeof running);
    memset(threads, 0, sizeof threads);
    rq_init();
    list_init(&all_threads);
    nr_runnable = 0;
    last_balance_ns = 0;
    ncpus = count;
    for (int i = 0; i < count; i++) {
        cpus[i].id = i;
        cpus[i].online = true;
        cpus[i].idle = &idle[i];
        cpus[i].cur = &running[i];   /* every CPU is busy */
        running[i].cpu = &cpus[i];
        running[i].state = T_RUNNING;
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
int main(void) {
    affinity_test();
    steal_test();
    balance_test();
    churn_test();
    printf("sched-host: %d checks passed (%s)\n", checks, sched_policy_name());
    return 0;
}