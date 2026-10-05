/* Per-CPU run queues: sched_setaffinity pins threads, busy threads spread over all CPUs. */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (c) printf("  [ok] %s\n", #c); else { printf("  [FAIL] %s (line %d)\n", #c, __LINE__); fails++; } } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static volatile unsigned long seen[64];

static void *spin(void *a) {
    double end = now() + 0.4;
    while (now() < end) { int c = sched_getcpu(); if (c >= 0 && c < 64) seen[c]++; for (volatile int i = 0; i < 2000; i++); }
    return 0;
}

int main(void) {
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    printf("cpus online: %d\n", n);
    cpu_set_t s;
    CHECK(sched_getaffinity(0, sizeof s, &s) == 0 && CPU_COUNT(&s) == n);
    for (int c = 0; c < n; c++) {
        CPU_ZERO(&s); CPU_SET(c, &s);
        CHECK(sched_setaffinity(0, sizeof s, &s) == 0);
        int ok = 1;
        for (int k = 0; k < 20; k++) { sched_yield(); if (sched_getcpu() != c) ok = 0; }
        printf("pinned to %d: %s\n", c, ok ? "stays" : "moved");
        CHECK(ok);
    }
    CPU_ZERO(&s); CPU_SET(n, &s);                         /* offline CPU only */
    CHECK(sched_setaffinity(0, sizeof s, &s) != 0);
    CPU_ZERO(&s); for (int c = 0; c < n; c++) CPU_SET(c, &s);
    CHECK(sched_setaffinity(0, sizeof s, &s) == 0);
    if (n > 1) {
        pthread_t t[8];
        int nt = n * 2 > 8 ? 8 : n * 2;
        for (int i = 0; i < nt; i++) pthread_create(&t[i], 0, spin, 0);
        for (int i = 0; i < nt; i++) pthread_join(t[i], 0);
        int used = 0;
        for (int c = 0; c < n; c++) { printf("cpu%d samples %lu\n", c, seen[c]); if (seen[c] > 100) used++; }
        CHECK(used == n);
    }
    if (fails) { printf("afftest: %d FAILED\n", fails); return 1; }
    puts("afftest: OK");
    return 0;
}
