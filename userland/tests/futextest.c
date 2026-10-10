/* futex(2) without the BKL: hashed bucket locks, value check vs. wake atomicity (lost-wakeup
 * stress), timeouts (relative, absolute BITSET), EAGAIN/EINVAL, CMP_REQUEUE, condvar broadcast,
 * and thread exit wakeups (CLONE_CHILD_CLEARTID via pthread_join). */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAIT_BITSET 9
#define FUTEX_PRIVATE 128

static long fx(volatile uint32_t *a, int op, uint32_t v, const struct timespec *ts, volatile uint32_t *a2, uint32_t v3) {
    long r = syscall(SYS_futex, a, op, v, ts, a2, v3);
    return r < 0 ? -errno : r;
}
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000ULL + t.tv_nsec; }
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("futextest: FAIL " __VA_ARGS__); putchar('\n'); fails++; } } while (0)

/* 1: a futex mutex (Drepper's mutex2) hammered by NT threads */
#define NT 4
#define ITERS 20000
static volatile uint32_t mtx;
static long counter;
static void lock(void) {
    uint32_t c = 0;
    if (__atomic_compare_exchange_n(&mtx, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    if (c != 2) c = __atomic_exchange_n(&mtx, 2, __ATOMIC_ACQUIRE);
    while (c) { fx(&mtx, FUTEX_WAIT | FUTEX_PRIVATE, 2, 0, 0, 0); c = __atomic_exchange_n(&mtx, 2, __ATOMIC_ACQUIRE); }
}
static void unlock(void) { if (__atomic_fetch_sub(&mtx, 1, __ATOMIC_RELEASE) != 1) { mtx = 0; fx(&mtx, FUTEX_WAKE | FUTEX_PRIVATE, 1, 0, 0, 0); } }
static void *hammer(void *a) { for (int i = 0; i < ITERS; i++) { lock(); counter++; unlock(); } return 0; }

/* 2: strict ping-pong over one word (every handoff needs a wakeup that must not be lost) */
#define ROUNDS 3000
static volatile uint32_t turn;
static void *pong(void *a) {
    uint32_t me = (uint32_t)(uintptr_t)a;
    for (int i = 0; i < ROUNDS; i++) {
        uint32_t t;
        while ((t = __atomic_load_n(&turn, __ATOMIC_ACQUIRE)) % 2 != me) fx(&turn, FUTEX_WAIT | FUTEX_PRIVATE, t, 0, 0, 0);
        __atomic_store_n(&turn, t + 1, __ATOMIC_RELEASE);
        fx(&turn, FUTEX_WAKE | FUTEX_PRIVATE, 1, 0, 0, 0);
    }
    return 0;
}

/* 3: condvar broadcast */
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int gen, arrived;
static void *cvwaiter(void *a) {
    for (int g = 1; g <= 200; g++) {
        pthread_mutex_lock(&m);
        arrived++;
        pthread_cond_broadcast(&cv);
        while (gen < g) pthread_cond_wait(&cv, &m);
        pthread_mutex_unlock(&m);
    }
    return 0;
}

/* 4: CMP_REQUEUE wakes one, moves the rest to rq2 */
static volatile uint32_t rq, rq2, parked;
static void *rqwaiter(void *a) {
    __atomic_add_fetch(&parked, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&rq, __ATOMIC_ACQUIRE) == 0) fx(&rq, FUTEX_WAIT | FUTEX_PRIVATE, 0, 0, 0, 0);
    return 0;
}
/* 5: requeued waiters really move to the second word */
static volatile uint32_t mq, mq2, mparked, mdone;
static void *mqwaiter(void *a) {
    __atomic_add_fetch(&mparked, 1, __ATOMIC_RELEASE);
    fx(&mq, FUTEX_WAIT | FUTEX_PRIVATE, 0, 0, 0, 0);
    __atomic_add_fetch(&mdone, 1, __ATOMIC_RELEASE);
    return 0;
}
static void *nothing(void *a) { return a; }

int main(void) {
    pthread_t t[NT];
    for (long i = 0; i < NT; i++) pthread_create(&t[i], 0, hammer, 0);
    for (int i = 0; i < NT; i++) pthread_join(t[i], 0);
    fprintf(stderr, "futextest: mutex done\n"); CHECK(counter == (long)NT * ITERS, "mutex counter=%ld", counter);

    pthread_create(&t[0], 0, pong, (void *)0); pthread_create(&t[1], 0, pong, (void *)1);
    pthread_join(t[0], 0); pthread_join(t[1], 0);
    fprintf(stderr, "futextest: ping-pong done\n"); CHECK(turn == 2 * ROUNDS, "ping-pong turn=%u", turn);

    for (long i = 0; i < NT; i++) pthread_create(&t[i], 0, cvwaiter, 0);
    for (int g = 1; g <= 200; g++) {
        pthread_mutex_lock(&m);
        while (arrived < NT * g) pthread_cond_wait(&cv, &m);
        gen = g; pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&m);
    }
    for (int i = 0; i < NT; i++) pthread_join(t[i], 0);

    fprintf(stderr, "futextest: condvar done\n");
    volatile uint32_t w = 5;
    CHECK(fx(&w, FUTEX_WAIT | FUTEX_PRIVATE, 4, 0, 0, 0) == -EAGAIN, "EAGAIN on mismatch");
    CHECK(fx((volatile uint32_t *)((char *)&w + 1), FUTEX_WAIT, 5, 0, 0, 0) == -EINVAL, "EINVAL on misaligned");
    CHECK(fx(&w, FUTEX_WAIT_BITSET, 5, 0, 0, 0) == -EINVAL, "EINVAL on empty bitset");
    CHECK(fx(&w, FUTEX_WAKE, 1, 0, 0, 0) == 0, "wake with no waiters");
    fprintf(stderr, "futextest: errors done\n");
    struct timespec rel = { 0, 30000000 };
    uint64_t t0 = now_ns();
    long r = fx(&w, FUTEX_WAIT | FUTEX_PRIVATE, 5, &rel, 0, 0);
    uint64_t dt = now_ns() - t0;
    CHECK(r == -ETIMEDOUT && dt >= 25000000 && dt < 2000000000ULL, "relative timeout r=%ld dt=%lu", r, (unsigned long)dt);
    fprintf(stderr, "futextest: relative done\n");
    struct timespec ab; clock_gettime(CLOCK_MONOTONIC, &ab);
    ab.tv_nsec += 30000000; if (ab.tv_nsec >= 1000000000) { ab.tv_sec++; ab.tv_nsec -= 1000000000; }
    t0 = now_ns();
    r = fx(&w, FUTEX_WAIT_BITSET | FUTEX_PRIVATE, 5, &ab, 0, ~0u);
    dt = now_ns() - t0;
    CHECK(r == -ETIMEDOUT && dt >= 25000000 && dt < 2000000000ULL, "absolute timeout r=%ld dt=%lu", r, (unsigned long)dt);
    fprintf(stderr, "futextest: absolute done\n");
    struct timespec past = { 0, 0 };
    CHECK(fx(&w, FUTEX_WAIT_BITSET | FUTEX_PRIVATE, 5, &past, 0, ~0u) == -ETIMEDOUT, "past deadline");

    fprintf(stderr, "futextest: timeouts done\n");
    for (long i = 0; i < NT; i++) pthread_create(&t[i], 0, rqwaiter, 0);
    while (__atomic_load_n(&parked, __ATOMIC_ACQUIRE) < NT) usleep(1000);
    usleep(20000);
    CHECK(fx(&rq, FUTEX_CMP_REQUEUE | FUTEX_PRIVATE, 1, (void *)(uintptr_t)INT32_MAX, &rq2, 7) == -EAGAIN, "CMP_REQUEUE mismatch");
    rq = 1;
    long woke = fx(&rq, FUTEX_CMP_REQUEUE | FUTEX_PRIVATE, 1, (void *)(uintptr_t)INT32_MAX, &rq2, 1);
    fx(&rq2, FUTEX_WAKE | FUTEX_PRIVATE, INT32_MAX, 0, 0, 0);     /* the requeued ones */
    for (int i = 0; i < NT; i++) pthread_join(t[i], 0);
    CHECK(woke >= 0 && woke <= NT, "CMP_REQUEUE woke=%ld", woke);

    for (long i = 0; i < NT; i++) pthread_create(&t[i], 0, mqwaiter, 0);
    while (__atomic_load_n(&mparked, __ATOMIC_ACQUIRE) < NT) usleep(1000);
    usleep(50000);                                /* all asleep in FUTEX_WAIT on mq */
    long moved = fx(&mq, FUTEX_CMP_REQUEUE | FUTEX_PRIVATE, 1, (void *)(uintptr_t)INT32_MAX, &mq2, 0);
    CHECK(moved == NT, "CMP_REQUEUE woke+moved=%ld", moved);
    usleep(50000);
    CHECK(__atomic_load_n(&mdone, __ATOMIC_ACQUIRE) == 1, "only one woken: %u", mdone);
    CHECK(fx(&mq, FUTEX_WAKE | FUTEX_PRIVATE, INT32_MAX, 0, 0, 0) == 0, "nobody left on mq");
    long w2 = fx(&mq2, FUTEX_WAKE | FUTEX_PRIVATE, INT32_MAX, 0, 0, 0);
    CHECK(w2 == NT - 1, "requeued waiters on mq2: %ld", w2);
    for (int i = 0; i < NT; i++) pthread_join(t[i], 0);
    CHECK(mdone == NT, "all done");
    fprintf(stderr, "futextest: requeue done\n");
    for (int i = 0; i < 300; i++) { pthread_t x; pthread_create(&x, 0, nothing, 0); pthread_join(x, 0); }

    if (fails) return 1;
    puts("futextest: OK");
    return 0;
}
