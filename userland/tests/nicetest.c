/* nice and real-time policies: CPU share of niced vs normal process on one CPU, SCHED_FIFO
 * thread starving a SCHED_OTHER one on the same CPU, getpriority/sched_* syscalls. */
#define _GNU_SOURCE
#include <pthread.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (c) printf("  [ok] %s\n", #c); else { printf("  [FAIL] %s (line %d)\n", #c, __LINE__); fails++; } } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static double cpu(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void pin(int c) { cpu_set_t s; CPU_ZERO(&s); CPU_SET(c, &s); sched_setaffinity(0, sizeof s, &s); }

/* child: pinned busy loop for 'secs', reports its CPU time in ms through the pipe */
static pid_t spinner(int cpu_id, int niceval, double secs, int fd) {
    pid_t p = fork();
    if (p == 0) {
        pin(cpu_id);
        setpriority(PRIO_PROCESS, 0, niceval);
        double c0 = cpu(), w0 = now(), end = w0 + secs;
        while (now() < end) for (volatile int i = 0; i < 5000; i++);
        int ms = (int)((cpu() - c0) * 1000);
        printf("  spinner nice %d: wall %.0f ms, cpu %d ms\n", niceval, (now() - w0) * 1000, ms);
        write(fd, &ms, sizeof ms);
        _exit(0);
    }
    return p;
}

static volatile int other_ran;
static void *other(void *a) { pin(1); double end = now() + 0.3; while (now() < end) other_ran++; return 0; }

int main(void) {
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    CHECK(getpriority(PRIO_PROCESS, 0) == 0);
    CHECK(setpriority(PRIO_PROCESS, 0, 5) == 0 && getpriority(PRIO_PROCESS, 0) == 5);
    setpriority(PRIO_PROCESS, 0, 0);
    CHECK(sched_get_priority_max(SCHED_FIFO) == 99 && sched_get_priority_min(SCHED_RR) == 1);
    CHECK(sched_get_priority_min(SCHED_OTHER) == 0 && sched_get_priority_max(SCHED_OTHER) == 0);
    errno = 0; CHECK(sched_get_priority_max(99) == -1 && errno == EINVAL);
    errno = 0; CHECK(sched_get_priority_min(-1) == -1 && errno == EINVAL);
    /* musl intentionally stubs sched_setscheduler/getscheduler with ENOSYS;
     * use the kernel ABI directly, as the FIFO scenario below already does. */
    struct sched_param bad = { .sched_priority = 0 };
    errno = 0; CHECK(syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &bad) == -1 && errno == EINVAL);
    bad.sched_priority = 100;
    errno = 0; CHECK(syscall(SYS_sched_setscheduler, 0, SCHED_RR, &bad) == -1 && errno == EINVAL);
    bad.sched_priority = 1;
    errno = 0; CHECK(syscall(SYS_sched_setscheduler, 0, SCHED_OTHER, &bad) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_sched_getscheduler, 0) == SCHED_OTHER);

    int pfd[2]; pipe(pfd);
    int cpu_id = n > 1 ? 1 : 0;
    pid_t a = spinner(cpu_id, 0, 1.0, pfd[1]), b = spinner(cpu_id, 10, 1.0, pfd[1]);
    waitpid(a, 0, 0); waitpid(b, 0, 0);
    int ma, mb; read(pfd[0], &ma, sizeof ma); read(pfd[0], &mb, sizeof mb);
    int hi = ma > mb ? ma : mb, lo = ma > mb ? mb : ma;
    printf("nice 0 vs nice 10 on cpu%d: %d ms vs %d ms\n", cpu_id, hi, lo);
    CHECK(hi * 2 > lo * 3);      /* nice 10 weighs ~9x less; leave room for coarse/lost ticks under emulation */

    if (n > 1) {
        /* a SCHED_FIFO thread spinning on cpu1 keeps an equal-priority FIFO thread (inherited
         * policy) on cpu1 from running until it drops back to SCHED_OTHER */
        pthread_t t;
        pin(1);
        struct sched_param sp = { .sched_priority = 10 };
        CHECK(syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &sp) == 0);
        CHECK(syscall(SYS_sched_getscheduler, 0) == SCHED_FIFO);
        pthread_create(&t, 0, other, 0);
        double end = now() + 0.25;
        while (now() < end) ;
        int during = other_ran;
        sp.sched_priority = 0;
        syscall(SYS_sched_setscheduler, 0, SCHED_OTHER, &sp);
        pthread_join(t, 0);
        printf("other thread iterations while FIFO spun: %d, after: %d\n", during, other_ran);
        CHECK(during == 0);
        CHECK(other_ran > 0);
        pin(0);
    }
    if (fails) { printf("nicetest: %d FAILED\n", fails); return 1; }
    puts("nicetest: OK");
    return 0;
}
