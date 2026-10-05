/* Per-CPU physical-page reuse through concurrent demand-zero mmap/munmap.
 * Host tests cover exhaustive pressure/coalescing; this exercises real mm/TLB paths. */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGES 24
#define ROUNDS 64
static int failures;
static long page_size;
#define CHECK(c) do { if (c) printf("  [ok] %s\n", #c); else { printf("  [FAIL] %s (line %d)\n", #c, __LINE__); __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED); } } while (0)
static long stat_value(const char *name) {
    FILE *f = fopen("/proc/vmstat", "r");
    if (!f) return -1;
    char key[64]; unsigned long value; long result = -1;
    while (fscanf(f, "%63s %lu", key, &value) == 2)
        if (!strcmp(key, name)) result = (long)value;
    fclose(f);
    return result;
}
static void *worker(void *arg) {
    long id = (long)arg;
    cpu_set_t mask;
    CPU_ZERO(&mask); CPU_SET(id, &mask);
    if (sched_setaffinity(0, sizeof mask, &mask)) {
        __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED); return 0;
    }
    for (int round = 0; round < ROUNDS; round++) {
        unsigned char *m = mmap(0, PAGES * page_size, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) { __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED); break; }
        for (int p = 0; p < PAGES; p++) {
            unsigned char marker = (unsigned char)(id * 32 + p + 1);
            if (m[p * page_size] || m[(p + 1) * page_size - 1])
                __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
            m[p * page_size] = marker;
            m[(p + 1) * page_size - 1] = marker ^ round;
        }
        for (int p = 0; p < PAGES; p++) {
            unsigned char marker = (unsigned char)(id * 32 + p + 1);
            if (m[p * page_size] != marker || m[(p + 1) * page_size - 1] != (marker ^ round))
                __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
        }
        if (munmap(m, PAGES * page_size)) __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
    }
    return 0;
}
int main(void) {
    page_size = sysconf(_SC_PAGESIZE);
    int online = (int)sysconf(_SC_NPROCESSORS_ONLN), count = online > 8 ? 8 : online;
    CHECK(page_size == 4096 && count > 0);
    long before = stat_value("pmm_pcpu_alloc_hits");
    CHECK(before >= 0);
    pthread_t threads[8];
    int created = 0;
    for (; created < count; created++)
        if (pthread_create(&threads[created], 0, worker, (void *)(long)created)) break;
    CHECK(created == count);
    for (int i = 0; i < created; i++) CHECK(pthread_join(threads[i], 0) == 0);
    long after = stat_value("pmm_pcpu_alloc_hits"), cached = stat_value("pmm_pcpu_cached");
    printf("pmm per-CPU hits: %ld -> %ld; cached pages: %ld\n", before, after, cached);
    CHECK(after > before);
    CHECK(cached >= 0 && cached <= online * 16);
    CHECK(stat_value("nr_free_pages") >= cached);
    CHECK(stat_value("pmm_pcpu_free_hits") > 0);
    if (__atomic_load_n(&failures, __ATOMIC_RELAXED)) {
        printf("pcputest: %d FAILED\n", failures); return 1;
    }
    puts("pcputest: OK");
    return 0;
}