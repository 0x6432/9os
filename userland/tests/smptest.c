/* SMP smoke test: parallel speed-up, CPU spread (getcpu), and mutex/atomic correctness. */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define WORK 40000000UL
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long shared_counter;
static long atomic_counter;
static int seen_cpu[64];

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *worker(void *arg) {
    volatile unsigned long x = 0;
    for (unsigned long i = 0; i < WORK; i++) {
        x += i ^ (x >> 3);
        if ((i & 0xffff) == 0) {
            int c = sched_getcpu();
            if (c >= 0 && c < 64) __atomic_store_n(&seen_cpu[c], 1, __ATOMIC_RELAXED);
        }
    }
    return (void *)x;
}

static void *locker(void *arg) {
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(&lock);
        shared_counter++;
        pthread_mutex_unlock(&lock);
        __atomic_fetch_add(&atomic_counter, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

static double run(int n, void *(*fn)(void *)) {
    pthread_t t[64];
    double t0 = now();
    for (int i = 0; i < n; i++) pthread_create(&t[i], NULL, fn, NULL);
    for (int i = 0; i < n; i++) pthread_join(t[i], NULL);
    return now() - t0;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    printf("smptest: %ld CPUs online, running %d threads\n", sysconf(_SC_NPROCESSORS_ONLN), n);
    double t1 = run(1, worker);
    for (int i = 0; i < 64; i++) seen_cpu[i] = 0;
    double tn = run(n, worker);
    double tl = run(n, locker);
    int cpus = 0;
    for (int i = 0; i < 64; i++) cpus += seen_cpu[i];
    printf("  1 thread: %.3f s   %d threads: %.3f s   speed-up %.2fx (ideal %d)\n", t1, n, tn, n * t1 / tn, n);
    printf("  threads ran on %d distinct CPUs; contended mutex phase %.3f s\n", cpus, tl);
    long want = 20000L * n;
    printf("  mutex counter %ld, atomic counter %ld (expected %ld)\n", shared_counter, atomic_counter, want);
    int ok = shared_counter == want && atomic_counter == want;
    printf("smptest: %s\n", ok ? "PASSED" : "FAILED");
    return !ok;
}
