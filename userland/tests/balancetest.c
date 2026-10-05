/* Balance queued normal threads while both allowed CPUs are busy with FIFO threads.
 * Without periodic balancing, idle stealing cannot help; widening an already-queued
 * thread also exercises nr_mig accounting. No throughput/timing ratios are required. */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define WORKERS 6
static int fails;
#define CHECK(c) do { if (c) printf("  [ok] %s\n", #c); else { printf("  [FAIL] %s (line %d)\n", #c, __LINE__); fails++; } } while (0)
static int ready, gates[WORKERS], tids[WORKERS], guard_ready, guard_stop, guard_expired, bad_cpu;

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static int pin(int tid, int second_cpu) {
    cpu_set_t set;
    CPU_ZERO(&set); CPU_SET(0, &set);
    if (second_cpu) CPU_SET(1, &set);
    return sched_setaffinity(tid, sizeof set, &set);
}
static int policy(int kind, int priority) {
    struct sched_param sp = { .sched_priority = priority };
    return syscall(SYS_sched_setscheduler, 0, kind, &sp);
}
static unsigned long balances(int cpu) {
    FILE *f = fopen("/proc/sched", "r");
    if (!f) return ~0UL;
    char line[256];
    unsigned long value = ~0UL;
    while (fgets(line, sizeof line, f)) {
        int id;
        if (sscanf(line, "cpu%d:", &id) == 1 && id == cpu) {
            char *p = strstr(line, " balances ");
            if (p) value = strtoul(p + strlen(" balances "), 0, 10);
        }
    }
    fclose(f);
    return value;
}
static void *worker(void *arg) {
    int id = (int)(long)arg;
    tids[id] = (int)syscall(SYS_gettid);
    __atomic_fetch_add(&ready, 1, __ATOMIC_RELEASE);
    /* Separate futexes ensure every worker is actually runnable after release.
     * musl condvar broadcast hands wakeups down a chain, which cannot advance
     * while the coordinator is FIFO and would leave only one worker queued. */
    while (!__atomic_load_n(&gates[id], __ATOMIC_ACQUIRE))
        syscall(SYS_futex, &gates[id], 128 /* WAIT_PRIVATE */, 0, 0, 0, 0);
    /* The coordinator narrows the masks again before dropping FIFO priority. */
    for (int i = 0; i < 40; i++) {
        if (sched_getcpu() != 0) __atomic_store_n(&bad_cpu, 1, __ATOMIC_RELAXED);
        sched_yield();
    }
    return 0;
}
static void *guard(void *unused) {
    cpu_set_t set;
    CPU_ZERO(&set); CPU_SET(1, &set);
    int ok = sched_setaffinity(0, sizeof set, &set) == 0 && policy(SCHED_FIFO, 20) == 0;
    __atomic_store_n(&guard_ready, ok ? 1 : -1, __ATOMIC_RELEASE);
    /* Safety deadline: even a broken coordinator cannot hold this CPU indefinitely. */
    /* TCG vCPUs on an oversubscribed host can pause for seconds while the
     * coordinator prints checks/takes the BKL. Keep the guard through setup
     * and cleanup, not just the nominal 400 ms observation interval. */
    double end = now() + 60;
    while (!__atomic_load_n(&guard_stop, __ATOMIC_ACQUIRE) && now() < end);
    if (!__atomic_load_n(&guard_stop, __ATOMIC_ACQUIRE))
        __atomic_store_n(&guard_expired, 1, __ATOMIC_RELEASE);
    policy(SCHED_OTHER, 0);
    return 0;
}
int main(void) {
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 2) { puts("balancetest: SKIP (needs >=2 CPUs)"); return 0; }
    CHECK(pin(0, 0) == 0);
    pthread_t workers[WORKERS], blocker;
    int count = 0;
    for (; count < WORKERS; count++)
        if (pthread_create(&workers[count], 0, worker, (void *)(long)count)) break;
    CHECK(count == WORKERS);
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) < count) sched_yield();
    /* Scheduling syscalls must resolve sibling TIDs, not only process leaders. */
    for (int i = 0; i < count; i++) {
        cpu_set_t set;
        struct sched_param sp;
        CHECK(sched_getaffinity(tids[i], sizeof set, &set) == 0 && CPU_COUNT(&set) == 1 && CPU_ISSET(0, &set));
        CHECK(syscall(SYS_sched_getscheduler, tids[i]) == SCHED_OTHER);
        CHECK(syscall(SYS_sched_getparam, tids[i], &sp) == 0 && sp.sched_priority == 0);
    }
    int have_guard = pthread_create(&blocker, 0, guard, 0) == 0;
    CHECK(have_guard);
    if (have_guard)
        while (!__atomic_load_n(&guard_ready, __ATOMIC_ACQUIRE)) sched_yield();
    CHECK(__atomic_load_n(&guard_ready, __ATOMIC_ACQUIRE) == 1);
    int fifo = policy(SCHED_FIFO, 10) == 0;
    CHECK(fifo);
    unsigned long before = balances(1);
    CHECK(before != ~0UL);
    for (int i = 0; i < count; i++) {
        __atomic_store_n(&gates[i], 1, __ATOMIC_RELEASE);
        syscall(SYS_futex, &gates[i], 129 /* WAKE_PRIVATE */, 1, 0, 0, 0);
    }                                  /* workers queue behind this FIFO thread on cpu0 */
    for (int i = 0; i < count; i++) CHECK(pin(tids[i], 1) == 0);
    if (fifo && have_guard && __atomic_load_n(&guard_ready, __ATOMIC_ACQUIRE) == 1) {
        double end = now() + 0.4;       /* do not sleep/yield: both CPUs must stay busy */
        while (now() < end);
        unsigned long after = balances(1);
        printf("cpu1 balance migrations: %lu -> %lu\n", before, after);
        CHECK(after != ~0UL && after > before);
    }
    /* Some workers are now queued on cpu1; narrow and move them back. Then widen
     * and narrow in-place to cover both nr_mig updates when rq_cpu stays cpu0. */
    for (int i = 0; i < count; i++) {
        CHECK(pin(tids[i], 0) == 0);
        CHECK(pin(tids[i], 1) == 0);
        CHECK(pin(tids[i], 0) == 0);
    }
    __atomic_store_n(&guard_stop, 1, __ATOMIC_RELEASE);
    CHECK(policy(SCHED_OTHER, 0) == 0);
    for (int i = 0; i < count; i++) CHECK(pthread_join(workers[i], 0) == 0);
    if (have_guard) CHECK(pthread_join(blocker, 0) == 0);
    CHECK(!__atomic_load_n(&guard_expired, __ATOMIC_ACQUIRE));
    CHECK(!__atomic_load_n(&bad_cpu, __ATOMIC_RELAXED));
    cpu_set_t set;
    CPU_ZERO(&set); for (int i = 0; i < n; i++) CPU_SET(i, &set);
    CHECK(sched_setaffinity(0, sizeof set, &set) == 0);
    if (fails) { printf("balancetest: %d FAILED\n", fails); return 1; }
    puts("balancetest: OK");
    return 0;
}