/* Pipe stress for the lock-free pipe path: atomic small writes with several producers and
 * consumers, poll() wakeups from another thread (lost-wakeup check), cross-process bulk copy, FIONREAD. */
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define REC 64
#define PER_PRODUCER 4000
#define NP 2
#define NC 2
static int pfd[2];
static volatile long got_recs, bad;

static void *producer(void *arg) {
    unsigned char r[REC];
    for (int i = 0; i < PER_PRODUCER; i++) {
        unsigned char s = 0;
        for (int k = 1; k < REC; k++) { r[k] = (unsigned char)((long)arg * 31 + i + k); s += r[k]; }
        r[0] = (unsigned char)-s;               /* record bytes sum to 0 */
        if (write(pfd[1], r, REC) != REC) __atomic_add_fetch(&bad, 1, __ATOMIC_RELAXED);
    }
    return 0;
}

static void *consumer(void *arg) {
    unsigned char buf[REC * 8];
    for (;;) {
        ssize_t n = read(pfd[0], buf, sizeof buf);
        if (n <= 0) break;
        if (n % REC) { __atomic_add_fetch(&bad, 1, __ATOMIC_RELAXED); continue; }
        for (ssize_t o = 0; o < n; o += REC) {
            unsigned char s = 0;
            for (int k = 0; k < REC; k++) s += buf[o + k];
            if (s) __atomic_add_fetch(&bad, 1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&got_recs, 1, __ATOMIC_RELAXED);
        }
    }
    return 0;
}

static int qfd[2];
static void *poker(void *arg) {
    for (int i = 0; i < 300; i++) { if (i % 3 == 0) usleep(200); char c = 'x'; write(qfd[1], &c, 1); }
    return 0;
}

static void *blocked_reader(void *arg) { char b; (void)arg; read(qfd[0], &b, 1); return 0; }
static void *sleeper(void *arg) { (void)arg; for (;;) usleep(100000); return 0; }

int main(void) {
    /* 0: FIONREAD reports the buffered byte count (swaybar's status line reader) */
    {
        int f[2], n = -1;
        pipe(f);
        write(f[1], "hello", 5);
        if (ioctl(f[0], FIONREAD, &n) || n != 5) { printf("pipetest: FIONREAD FAIL n=%d\n", n); return 1; }
        close(f[0]); close(f[1]);
    }
    /* 1: multi-producer / multi-consumer */
    pipe(pfd);
    pthread_t p[NP], c[NC];
    for (long i = 0; i < NC; i++) pthread_create(&c[i], 0, consumer, 0);
    for (long i = 0; i < NP; i++) pthread_create(&p[i], 0, producer, (void *)i);
    for (int i = 0; i < NP; i++) pthread_join(p[i], 0);
    close(pfd[1]);
    for (int i = 0; i < NC; i++) pthread_join(c[i], 0);
    if (bad || got_recs != NP * PER_PRODUCER) { printf("pipetest: mpmc FAIL bad=%ld recs=%ld\n", bad, got_recs); return 1; }

    /* 2: poll wakeups from a lock-free writer on another CPU */
    pipe(qfd);
    pthread_t t;
    pthread_create(&t, 0, poker, 0);
    int seen = 0;
    while (seen < 300) {
        struct pollfd pf = { qfd[0], POLLIN, 0 };
        int r = poll(&pf, 1, 5000);
        if (r != 1) { printf("pipetest: poll FAIL r=%d seen=%d\n", r, seen); return 1; }
        char b[64];
        ssize_t n = read(qfd[0], b, sizeof b);
        if (n > 0) seen += (int)n;
    }
    pthread_join(t, 0);

    /* 3: cross-process bulk transfer */
    int x[2]; pipe(x);
    pid_t pid = fork();
    if (pid == 0) {
        close(x[0]);
        static unsigned char blk[8192];
        for (int i = 0; i < 256; i++) { memset(blk, i, sizeof blk); if (write(x[1], blk, sizeof blk) != sizeof blk) _exit(1); }
        _exit(0);
    }
    close(x[1]);
    uint64_t sum = 0, total = 0;
    unsigned char b[3000];
    ssize_t n;
    while ((n = read(x[0], b, sizeof b)) > 0) { for (ssize_t i = 0; i < n; i++) sum += b[i]; total += n; }
    int st; waitpid(pid, &st, 0);
    uint64_t want = 0; for (int i = 0; i < 256; i++) want += (uint64_t)i * 8192;
    if (total != 256 * 8192 || sum != want || !WIFEXITED(st) || WEXITSTATUS(st)) {
        printf("pipetest: bulk FAIL total=%lu sum=%lu\n", (unsigned long)total, (unsigned long)sum); return 1;
    }
    /* 4: exit() while sibling threads sleep in the kernel (pipe read, nanosleep): the process
     * must not tear down its mm under them (sway/foot at shutdown) */
    for (int it = 0; it < 40; it++) {
        pid_t c = fork();
        if (c == 0) {
            pipe(qfd);
            pthread_t s1, s2, s3;
            pthread_create(&s1, 0, blocked_reader, 0);
            pthread_create(&s2, 0, blocked_reader, 0);
            pthread_create(&s3, 0, sleeper, 0);
            usleep(2000 + (it % 4) * 1000);
            exit(7);
        }
        int cst; waitpid(c, &cst, 0);
        if (!WIFEXITED(cst) || WEXITSTATUS(cst) != 7) { printf("pipetest: exit FAIL st=%x\n", cst); return 1; }
    }
    puts("pipetest: OK");
    return 0;
}
