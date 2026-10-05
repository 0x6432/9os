/* Deadline/NOHZ regressions: idle AP tick suppression, elapsed idle accounting,
 * relative/absolute/periodic timerfds, timeout wakeups and remotely armed timers. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (c) printf("  [ok] %s\n", #c); else { printf("  [FAIL] %s (line %d)\n", #c, __LINE__); fails++; } } while (0)
struct counters { unsigned long ticks, idle, nohz, local; };
static int snapshot(struct counters *out) {
    FILE *f = fopen("/proc/sched", "r"); if (!f) return -1;
    char line[512]; int count = 0;
    memset(out, 0, sizeof(*out) * 16);
    while (fgets(line, sizeof line, f)) {
        int id; unsigned long hw, switches, steals, balances, coordinated; int rq;
        struct counters c;
        if (sscanf(line, "cpu%d: hwid 0x%lx ticks %lu idle %lu switches %lu rq %d steals %lu balances %lu local %lu coordinated %lu nohz %lu",
                   &id, &hw, &c.ticks, &c.idle, &switches, &rq, &steals, &balances, &c.local, &coordinated, &c.nohz) == 11 && id >= 0 && id < 16)
            { out[id] = c; count++; }
    }
    fclose(f); return count;
}
static int pin(int cpu) { cpu_set_t m; CPU_ZERO(&m); CPU_SET(cpu, &m); return sched_setaffinity(0, sizeof m, &m); }
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
static struct timespec ns_ts(uint64_t ns) { return (struct timespec){ ns / 1000000000, ns % 1000000000 }; }
static int remote_fd, remote_result;
static void *remote_arm(void *arg) {
    (void)arg;
    if (pin(1)) { remote_result = -1; return 0; }
    usleep(20000);
    struct itimerspec it = { .it_value = {0, 1000000} };
    remote_result = timerfd_settime(remote_fd, 0, &it, 0);
    return 0;
}
static int group_ready, group_stop, group_bad;
static void *affinity_group(void *arg) {
    (void)arg;
    cpu_set_t mask; CPU_ZERO(&mask); CPU_SET(1, &mask); CPU_SET(2, &mask);
    if (sched_setaffinity(0, sizeof mask, &mask)) __atomic_fetch_add(&group_bad, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&group_ready, 1, __ATOMIC_RELEASE);
    uint64_t guard = now_ns() + 10000000000ULL;
    while (!__atomic_load_n(&group_stop, __ATOMIC_ACQUIRE) && now_ns() < guard)
        for (volatile int i = 0; i < 1000; i++);
    return 0;
}
static void ineligible_donor_test(void) {
    pthread_t workers[8]; int made = 0;
    for (; made < 8; made++) if (pthread_create(&workers[made], 0, affinity_group, 0)) break;
    CHECK(made == 8);
    uint64_t guard = now_ns() + 3000000000ULL;
    while (__atomic_load_n(&group_ready, __ATOMIC_ACQUIRE) < made && now_ns() < guard) usleep(5000);
    CHECK(__atomic_load_n(&group_ready, __ATOMIC_ACQUIRE) == made && !group_bad);
    struct counters before[16], after[16];
    CHECK(snapshot(before) > 3);
    uint64_t start = now_ns(); usleep(250000); uint64_t elapsed = now_ns() - start;
    CHECK(snapshot(after) > 3);
    printf("ineligible donor hint: cpu3 ticks +%lu, idle ms +%lu\n",
           after[3].ticks - before[3].ticks, after[3].idle - before[3].idle);
    CHECK(after[3].ticks - before[3].ticks < elapsed / 2000000);
    CHECK(after[3].idle - before[3].idle > elapsed / 4000000);
    __atomic_store_n(&group_stop, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < made; i++) CHECK(pthread_join(workers[i], 0) == 0);
}
int main(void) {
    int online = sysconf(_SC_NPROCESSORS_ONLN);
    CHECK(pin(0) == 0);
    struct counters before[16], after[16];
    CHECK(snapshot(before) == online);
    uint64_t start = now_ns(); usleep(300000); uint64_t elapsed = now_ns() - start;
    CHECK(elapsed >= 290000000 && elapsed < 5000000000ULL);
    CHECK(snapshot(after) == online);
    for (int i = 1; i < online && i < 16; i++) {
        printf("idle cpu%d: ticks +%lu, idle ms +%lu, deadline sleeps +%lu\n", i,
               after[i].ticks - before[i].ticks, after[i].idle - before[i].idle, after[i].nohz - before[i].nohz);
        CHECK(after[i].ticks - before[i].ticks < elapsed / 2000000); /* less than half of periodic 1kHz */
        CHECK(after[i].idle - before[i].idle > elapsed / 4000000); /* live, still-halted span included */
        CHECK(after[i].nohz > 0);
    }
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC); CHECK(fd >= 0);
    uint64_t count = 0;
    errno = 0; CHECK(read(fd, &count, sizeof count) == -1 && errno == EAGAIN);
    struct itimerspec it = { .it_value = {0, 15000000} };
    start = now_ns(); CHECK(timerfd_settime(fd, 0, &it, 0) == 0);
    struct pollfd p = {fd, POLLIN, 0}; CHECK(poll(&p, 1, 2000) == 1 && (p.revents & POLLIN));
    elapsed = now_ns() - start; CHECK(elapsed >= 14000000 && elapsed < 2000000000ULL);
    CHECK(read(fd, &count, sizeof count) == sizeof count && count == 1);
    it.it_value = ns_ts(now_ns() + 12000000); CHECK(timerfd_settime(fd, TFD_TIMER_ABSTIME, &it, 0) == 0);
    CHECK(poll(&p, 1, 2000) == 1); CHECK(read(fd, &count, sizeof count) == sizeof count && count == 1);
    it.it_interval = ns_ts(5000000); it.it_value = it.it_interval;
    CHECK(timerfd_settime(fd, 0, &it, 0) == 0); usleep(60000);
    CHECK(read(fd, &count, sizeof count) == sizeof count && count >= 8);
    struct itimerspec old;
    memset(&it, 0, sizeof it); CHECK(timerfd_settime(fd, 0, &it, &old) == 0);
    CHECK(old.it_interval.tv_nsec == 5000000);
    struct itimerspec cur; CHECK(timerfd_gettime(fd, &cur) == 0 && cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec == 0);
    CHECK(poll(&p, 1, 20) == 0); /* disarmed timer must not revive */
    start = now_ns(); CHECK(poll(0, 0, 15) == 0); CHECK(now_ns() - start >= 14000000);
    if (online > 1) {
        remote_fd = fd; remote_result = -1; pthread_t t;
        int created = pthread_create(&t, 0, remote_arm, 0); CHECK(created == 0);
        if (!created) {
            CHECK(poll(&p, 1, 2000) == 1); CHECK(pthread_join(t, 0) == 0 && remote_result == 0);
            CHECK(read(fd, &count, sizeof count) == sizeof count && count == 1);
        }
    }
    close(fd);
    if (online >= 4) ineligible_donor_test();
    if (fails) { printf("idletest: %d FAILED\n", fails); return 1; }
    puts("idletest: OK"); return 0;
}
