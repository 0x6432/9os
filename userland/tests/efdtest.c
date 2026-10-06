/* eventfd without the BKL: concurrent writers/readers (counter CAS), semaphore mode, EAGAIN,
 * blocking on overflow, and poll() wakeups from another thread. */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define NW 2
#define NR 2
#define PER 5000
static int efd;
static volatile long got;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("efdtest: FAIL " __VA_ARGS__); putchar('\n'); fails++; } } while (0)

static void *writer(void *a) { uint64_t one = 1; for (int i = 0; i < PER; i++) if (write(efd, &one, 8) != 8) fails++; return 0; }
static void *reader(void *a) {
    uint64_t v;
    for (;;) {
        if (__atomic_load_n(&got, __ATOMIC_ACQUIRE) >= NW * PER) break;
        struct pollfd p = { efd, POLLIN, 0 };
        if (poll(&p, 1, 50) <= 0) continue;
        if (read(efd, &v, 8) == 8) __atomic_add_fetch(&got, (long)v, __ATOMIC_ACQ_REL);
    }
    return 0;
}
static void *late(void *a) { usleep(30000); uint64_t v = 3; write(*(int *)a, &v, 8); return 0; }

int main(void) {
    efd = eventfd(0, EFD_SEMAPHORE | EFD_NONBLOCK);
    pthread_t w[NW], r[NR];
    for (int i = 0; i < NR; i++) pthread_create(&r[i], 0, reader, 0);
    for (int i = 0; i < NW; i++) pthread_create(&w[i], 0, writer, 0);
    for (int i = 0; i < NW; i++) pthread_join(w[i], 0);
    for (int i = 0; i < NR; i++) pthread_join(r[i], 0);
    CHECK(got == NW * PER, "mpmc got=%ld", got);
    uint64_t v;
    CHECK(read(efd, &v, 8) == -1 && errno == EAGAIN, "EAGAIN when empty");
    CHECK(read(efd, &v, 4) == -1 && errno == EINVAL, "EINVAL short read");
    close(efd);

    int b = eventfd(0, 0);                       /* blocking read woken by a late writer */
    pthread_t t; pthread_create(&t, 0, late, &b);
    CHECK(read(b, &v, 8) == 8 && v == 3, "blocking read v=%lu", (unsigned long)v);
    pthread_join(t, 0);
    v = UINT64_MAX - 2;                           /* counter near the limit: nonblocking write fails */
    CHECK(write(b, &v, 8) == 8, "big write");
    int nb = eventfd(0, EFD_NONBLOCK);
    v = UINT64_MAX - 1; CHECK(write(nb, &v, 8) == 8, "max write");
    v = 1; CHECK(write(nb, &v, 8) == -1 && errno == EAGAIN, "overflow EAGAIN");
    v = UINT64_MAX; CHECK(write(nb, &v, 8) == -1 && errno == EINVAL, "UINT64_MAX EINVAL");
    CHECK(read(nb, &v, 8) == 8 && v == UINT64_MAX - 1, "read max");
    if (fails) return 1;
    puts("efdtest: OK");
    return 0;
}
