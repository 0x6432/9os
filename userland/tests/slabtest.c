/* Real kernel object churn on each CPU: cached file/inode/pipe/eventfd/socket
 * objects must be clean on reuse and share no stale data or descriptors. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
static int fails;
#define CHECK(c) do { if (c) printf("  [ok] %s\n", #c); else { printf("  [FAIL] %s (line %d)\n", #c, __LINE__); __atomic_fetch_add(&fails, 1, __ATOMIC_RELAXED); } } while (0)
static long stat_value(const char *name) {
    FILE *f = fopen("/proc/vmstat", "r"); if (!f) return -1;
    char key[64]; unsigned long value; long result = -1;
    while (fscanf(f, "%63s %lu", key, &value) == 2) if (!strcmp(key, name)) result = value;
    fclose(f); return result;
}
static void bad(void) { __atomic_fetch_add(&fails, 1, __ATOMIC_RELAXED); }
static void *worker(void *arg) {
    int id = (intptr_t)arg; cpu_set_t m; CPU_ZERO(&m); CPU_SET(id, &m);
    if (sched_setaffinity(0, sizeof m, &m)) { bad(); return 0; }
    for (int round = 0; round < 96; round++) {
        int pf[2]; if (pipe(pf)) { bad(); return 0; }
        unsigned char in[64], out[64]; memset(in, id + round, sizeof in);
        if (write(pf[1], in, sizeof in) != sizeof in || read(pf[0], out, sizeof out) != sizeof out || memcmp(in, out, sizeof in)) bad();
        close(pf[1]); if (read(pf[0], out, 1) != 0) bad(); close(pf[0]);
        int e = eventfd(0, EFD_NONBLOCK); if (e < 0) { bad(); return 0; }
        uint64_t v; errno = 0; if (read(e, &v, sizeof v) != -1 || errno != EAGAIN) bad();
        v = id + round + 1;
        if (write(e, &v, sizeof v) != sizeof v) bad();
        uint64_t got = 0; if (read(e, &got, sizeof got) != sizeof got || got != v) bad();
        close(e);
        int sf[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, sf)) { bad(); return 0; }
        if (write(sf[0], in, sizeof in) != sizeof in || read(sf[1], out, sizeof out) != sizeof out || memcmp(in, out, sizeof in)) bad();
        close(sf[0]); close(sf[1]);
        if (round % 8 == 0) sched_yield();
    }
    return 0;
}
int main(void) {
    int online = sysconf(_SC_NPROCESSORS_ONLN), count = online > 8 ? 8 : online;
    long before = stat_value("slab_pcpu_alloc_hits"); CHECK(before >= 0);
    pthread_t threads[8]; int created = 0;
    for (; created < count; created++) if (pthread_create(&threads[created], 0, worker, (void *)(intptr_t)created)) break;
    CHECK(created == count);
    for (int i = 0; i < created; i++) CHECK(pthread_join(threads[i], 0) == 0);
    long after = stat_value("slab_pcpu_alloc_hits"), cached = stat_value("slab_pcpu_cached");
    printf("slab per-CPU hits %ld -> %ld, cached objects %ld\n", before, after, cached);
    CHECK(after > before && cached >= 0);
    CHECK(stat_value("slab_pcpu_free_hits") > 0 && stat_value("slab_pcpu_drained") > 0);
    if (fails) { printf("slabtest: %d FAILED\n", fails); return 1; }
    puts("slabtest: OK"); return 0;
}
