/* Informational wakeup benchmark and actual readiness/progress regression.
 * Private idle eventfds must not report readiness for unrelated transfers.
 * Wall-time/context-switch measurements are never correctness thresholds. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define N 24
#if ATOMIC_INT_LOCK_FREE != 2
#error "The timeout diagnostic requires lock-free atomic int"
#endif
static int fd[N], ready, failed;
static long switches[N];
static atomic_int phase, created;

/* No progress-printing syscalls in the measured path. On a real stall, preserve
 * which blocking phase failed; never retry or convert the timeout to success. */
static void timeout_handler(int sig) {
    (void)sig;
    switch (atomic_load_explicit(&phase, memory_order_relaxed)) {
#define STALLED(n, text) case n: (void)write(2, text, sizeof(text) - 1); break
        STALLED(0, "pollwakebench: TIMEOUT waiting for private pollers to start\n");
        STALLED(1, "pollwakebench: TIMEOUT in unrelated eventfd transfers\n");
        STALLED(2, "pollwakebench: TIMEOUT waking private pollers\n");
        STALLED(3, "pollwakebench: TIMEOUT joining private pollers\n");
#undef STALLED
    }
    char counts[] = "pollwakebench: created=00 ready=00\n";
    int c = atomic_load_explicit(&created, memory_order_relaxed);
    int r = __atomic_load_n(&ready, __ATOMIC_RELAXED);
    counts[23] = '0' + c / 10; counts[24] = '0' + c % 10;
    counts[32] = '0' + r / 10; counts[33] = '0' + r % 10;
    (void)write(2, counts, sizeof(counts) - 1);
    _exit(124);
}
static void fail(void) { __atomic_add_fetch(&failed, 1, __ATOMIC_RELAXED); }
static long long ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock_gettime"); exit(2); }
    return (long long)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void *sleeper(void *arg) {
    int i = (int)(intptr_t)arg;
    struct rusage before = {0}, after = {0};
    if (getrusage(RUSAGE_THREAD, &before)) fail();
    __atomic_add_fetch(&ready, 1, __ATOMIC_RELEASE);
    struct pollfd p = {fd[i], POLLIN, 0};
    int r;
    do { r = poll(&p, 1, -1); } while (r < 0 && errno == EINTR);
    uint64_t v = 0;
    if (r != 1 || !(p.revents & POLLIN) || read(fd[i], &v, 8) != 8 || v != 1) fail();
    if (getrusage(RUSAGE_THREAD, &after)) fail();
    switches[i] = after.ru_nvcsw + after.ru_nivcsw - before.ru_nvcsw - before.ru_nivcsw;
    return NULL;
}
int main(void) {
    if (signal(SIGALRM, timeout_handler) == SIG_ERR) { perror("signal"); return 2; }
    alarm(45);
    setvbuf(stdout, NULL, _IOLBF, 0);
    pthread_t t[N];
    for (int i = 0; i < N; i++) {
        fd[i] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (fd[i] < 0 || pthread_create(&t[i], NULL, sleeper, (void *)(intptr_t)i)) return 2;
        atomic_store_explicit(&created, i + 1, memory_order_relaxed);
    }
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != N) usleep(1000);
    usleep(20000);
    int other = eventfd(0, EFD_CLOEXEC);
    if (other < 0) return 2;
    struct rusage a = {0}, b = {0};
    if (getrusage(RUSAGE_THREAD, &a)) fail();
    atomic_store_explicit(&phase, 1, memory_order_relaxed);
    long long start = ns();
    for (int i = 0; i < 1000; i++) {
        uint64_t one = 1, v = 0;
        if (write(other, &one, 8) != 8 || read(other, &v, 8) != 8 || v != 1) fail();
    }
    long long elapsed = ns() - start;
    if (getrusage(RUSAGE_THREAD, &b)) fail();
    atomic_store_explicit(&phase, 2, memory_order_relaxed);
    for (int i = 0; i < N; i++) {
        uint64_t one = 1;
        if (write(fd[i], &one, 8) != 8) fail();
    }
    atomic_store_explicit(&phase, 3, memory_order_relaxed);
    long total = 0;
    for (int i = 0; i < N; i++) {
        if (pthread_join(t[i], NULL)) return 2;
        total += switches[i];
        close(fd[i]);
    }
    close(other);
    printf("pollwakebench: transfers=1000 idle_waiters=%d wall_ms=%lld "
           "producer_switches=%ld waiter_switches=%ld failures=%d\n", N, elapsed / 1000000,
           b.ru_nvcsw + b.ru_nivcsw - a.ru_nvcsw - a.ru_nivcsw, total,
           __atomic_load_n(&failed, __ATOMIC_RELAXED));
    return __atomic_load_n(&failed, __ATOMIC_RELAXED) ? 1 : 0;
}
