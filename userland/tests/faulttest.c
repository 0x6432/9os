/* Concurrent page-fault / user-copy stress: threads of one process demand-fault a shared
 * region, cycle private mmap/munmap, have lock-free syscalls write into untouched pages,
 * and fork in the middle (COW against running faulters). Exercises the mm lock. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NT 4
#define SHARED_PAGES 512
#define ROUNDS 40
static char *shared;
static long pg;
static volatile int fail;

static void *worker(void *arg) {
    long id = (long)arg;
    for (int r = 0; r < ROUNDS && !fail; r++) {
        /* interleaved pages of the shared region: neighbours fault concurrently */
        for (int p = (int)id; p < SHARED_PAGES; p += NT) shared[p * pg + r % 64] = (char)(id + r);
        /* private churn */
        char *m = mmap(0, 32 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) { fail = 1; break; }
        for (int p = 0; p < 32; p++) {
            if (m[p * pg]) { fail = 2; break; }        /* demand-zero */
            m[p * pg] = (char)p;
        }
        /* lock-free syscall copying out into a page never touched before */
        struct timespec *ts = (struct timespec *)(m + 31 * pg + 64);
        if (clock_gettime(CLOCK_MONOTONIC, ts) || ts->tv_sec < 0) fail = 3;
        for (int p = 0; p < 31; p++) if (m[p * pg] != (char)p) fail = 4;
        if (munmap(m, 32 * pg)) fail = 5;
    }
    return 0;
}

int main(void) {
    pg = sysconf(_SC_PAGESIZE);
    shared = mmap(0, SHARED_PAGES * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) { puts("faulttest: mmap failed"); return 1; }
    pthread_t t[NT];
    for (long i = 0; i < NT; i++) pthread_create(&t[i], 0, worker, (void *)i);
    int forks = 0;
    for (int k = 0; k < 4; k++) {
        usleep(2000);
        pid_t c = fork();
        if (c == 0) {
            /* child: snapshot must be self-consistent, writes must not leak to the parent */
            for (int p = 0; p < SHARED_PAGES; p++) shared[p * pg + 100] = 0x5a;
            _exit(0);
        }
        int st;
        if (c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && !WEXITSTATUS(st)) forks++;
    }
    for (int i = 0; i < NT; i++) pthread_join(t[i], 0);
    for (int p = 0; p < SHARED_PAGES; p++) {
        if (shared[p * pg + 100] == 0x5a) { fail = 6; break; }
        if (shared[p * pg + (ROUNDS - 1) % 64] != (char)(p % NT + ROUNDS - 1)) { fail = 7; break; }
    }
    if (fail || forks != 4) { printf("faulttest: FAIL %d forks %d\n", fail, forks); return 1; }
    puts("faulttest: OK");
    return 0;
}
