/* M33 signals and process APIs: queued real-time signals with siginfo (sigqueue, ordering,
 * standard-signal coalescing, SA_SIGINFO handlers, rt_sigqueueinfo), POSIX timers (SIGEV_SIGNAL
 * with si_value/overruns, SIGEV_NONE, TIMER_ABSTIME, SIGEV_THREAD via musl, i.e.
 * SIGEV_THREAD_ID), robust futexes (thread and process-shared owner death -> EOWNERDEAD),
 * clone3 with CLONE_PIDFD, pidfd_open/pidfd_send_signal/pidfd_getfd/poll, and waitid (P_PID,
 * P_PGID, P_ALL, P_PIDFD, WNOHANG, WNOWAIT, WSTOPPED, WCONTINUED). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("sigqtest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" (errno %d)\n", errno); fflush(stdout); fails++; } } while (0)
#ifndef SYS_clone3
#define SYS_clone3 435
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif
#ifndef P_PIDFD
#define P_PIDFD 3
#endif
#ifndef CLONE_PIDFD
#define CLONE_PIDFD 0x1000
#endif
#ifndef SIGEV_THREAD_ID
#define SIGEV_THREAD_ID 4
#endif

static const struct timespec ts0 = { 0, 0 }, ts1s = { 1, 0 };

/* ---- queued signals ---- */
static volatile int h_val, h_code, h_pid, h_count;
static void h_info(int sig, siginfo_t *si, void *uc) { (void)sig; (void)uc; h_val = si->si_value.sival_int; h_code = si->si_code; h_pid = si->si_pid; h_count++; }

static void test_queue(void) {
    sigset_t s; sigemptyset(&s);
    int r1 = SIGRTMIN, r2 = SIGRTMIN + 2;
    sigaddset(&s, r1); sigaddset(&s, r2); sigaddset(&s, SIGUSR1);
    sigprocmask(SIG_BLOCK, &s, NULL);
    for (int v = 1; v <= 3; v++) CHECK(sigqueue(getpid(), r2, (union sigval){ .sival_int = 100 + v }) == 0, "sigqueue r2");
    for (int v = 1; v <= 3; v++) CHECK(sigqueue(getpid(), r1, (union sigval){ .sival_int = v }) == 0, "sigqueue r1");
    kill(getpid(), SIGUSR1); kill(getpid(), SIGUSR1);
    sigset_t pend; sigpending(&pend);
    CHECK(sigismember(&pend, r1) && sigismember(&pend, r2) && sigismember(&pend, SIGUSR1), "sigpending");
    siginfo_t si;
    CHECK(sigtimedwait(&s, &si, &ts0) == SIGUSR1 && si.si_code == SI_USER && si.si_pid == getpid(), "SIGUSR1 code %d", si.si_code);
    for (int v = 1; v <= 3; v++) {
        int g = sigtimedwait(&s, &si, &ts0);
        CHECK(g == r1 && si.si_code == SI_QUEUE && si.si_value.sival_int == v && si.si_pid == getpid() && si.si_uid == getuid(),
              "r1 #%d: sig %d code %d val %d", v, g, si.si_code, si.si_value.sival_int);
    }
    for (int v = 1; v <= 3; v++) {
        int g = sigtimedwait(&s, &si, &ts0);
        CHECK(g == r2 && si.si_value.sival_int == 100 + v, "r2 #%d: sig %d val %d", v, g, si.si_value.sival_int);
    }
    errno = 0;
    CHECK(sigtimedwait(&s, &si, &ts0) < 0 && errno == EAGAIN, "queue empty");      /* SIGUSR1 coalesced */
    struct sigaction sa = { .sa_sigaction = h_info, .sa_flags = SA_SIGINFO };
    sigaction(r1, &sa, NULL);
    sigqueue(getpid(), r1, (union sigval){ .sival_int = 77 });
    sigqueue(getpid(), r1, (union sigval){ .sival_int = 78 });
    sigprocmask(SIG_UNBLOCK, &s, NULL);
    CHECK(h_count == 2 && h_val == 78 && h_code == SI_QUEUE && h_pid == getpid(), "handler count %d val %d code %d", h_count, h_val, h_code);
    signal(r1, SIG_DFL);
    siginfo_t q; memset(&q, 0, sizeof q);
    q.si_signo = r1; q.si_code = SI_QUEUE; q.si_value.sival_int = 5;
    sigprocmask(SIG_BLOCK, &s, NULL);
    CHECK(syscall(SYS_rt_sigqueueinfo, getpid(), r1, &q) == 0, "rt_sigqueueinfo self");
    CHECK(sigtimedwait(&s, &si, &ts0) == r1 && si.si_value.sival_int == 5, "rt_sigqueueinfo delivered");
    pid_t c = fork();
    if (!c) { pause(); _exit(0); }
    q.si_code = SI_KERNEL;
    errno = 0;
    CHECK(syscall(SYS_rt_sigqueueinfo, c, SIGTERM, &q) < 0 && errno == EPERM, "positive si_code to other process");
    kill(c, SIGKILL); waitpid(c, NULL, 0);
    q.si_code = SI_QUEUE; q.si_value.sival_int = 9;
    CHECK(syscall(SYS_rt_tgsigqueueinfo, getpid(), gettid(), r2, &q) == 0, "rt_tgsigqueueinfo");
    CHECK(sigtimedwait(&s, &si, &ts0) == r2 && si.si_value.sival_int == 9, "tgsigqueueinfo delivered");
    sigprocmask(SIG_UNBLOCK, &s, NULL);
}

/* ---- POSIX timers ---- */
static volatile int thr_calls, thr_val;
static void thr_cb(union sigval v) { thr_val = v.sival_int; __atomic_add_fetch(&thr_calls, 1, __ATOMIC_SEQ_CST); }

static void test_timers(void) {
    int rs = SIGRTMIN + 1;
    sigset_t s; sigemptyset(&s); sigaddset(&s, rs);
    sigprocmask(SIG_BLOCK, &s, NULL);
    timer_t t;
    struct sigevent ev = { .sigev_notify = SIGEV_SIGNAL, .sigev_signo = rs, .sigev_value.sival_int = 42 };
    CHECK(timer_create(CLOCK_MONOTONIC, &ev, &t) == 0, "timer_create");
    struct itimerspec it = { .it_value = { 0, 20000000 }, .it_interval = { 0, 10000000 } }, cur;
    CHECK(timer_settime(t, 0, &it, NULL) == 0, "settime");
    CHECK(timer_gettime(t, &cur) == 0 && cur.it_interval.tv_nsec == 10000000 && cur.it_value.tv_nsec <= 20000000, "gettime");
    siginfo_t si;
    int g = sigtimedwait(&s, &si, &ts1s);
    CHECK(g == rs && si.si_code == SI_TIMER && si.si_value.sival_int == 42, "timer signal %d code %d val %d", g, si.si_code, si.si_value.sival_int);
    usleep(120000);
    g = sigtimedwait(&s, &si, &ts1s);
    CHECK(g == rs && si.si_overrun >= 5, "overrun in siginfo %d", si.si_overrun);
    usleep(60000);
    CHECK(timer_getoverrun(t) >= 0, "getoverrun");
    struct itimerspec old, off = { 0 };
    CHECK(timer_settime(t, 0, &off, &old) == 0 && old.it_interval.tv_nsec == 10000000, "disarm returns old");
    while (sigtimedwait(&s, &si, &ts0) > 0) ;
    CHECK(timer_gettime(t, &cur) == 0 && !cur.it_value.tv_sec && !cur.it_value.tv_nsec, "disarmed");
    CHECK(timer_delete(t) == 0, "delete");
    errno = 0;
    CHECK(syscall(SYS_timer_gettime, (long)(intptr_t)t, &cur) < 0 && errno == EINVAL, "deleted id");
    ev.sigev_notify = SIGEV_NONE;
    CHECK(timer_create(CLOCK_REALTIME, &ev, &t) == 0, "SIGEV_NONE");
    it = (struct itimerspec){ .it_value = { 5, 0 } };
    timer_settime(t, 0, &it, NULL);
    usleep(20000);
    CHECK(timer_gettime(t, &cur) == 0 && cur.it_value.tv_sec == 4, "SIGEV_NONE remaining %ld", (long)cur.it_value.tv_sec);
    timer_delete(t);
    sigset_t a; sigemptyset(&a); sigaddset(&a, SIGALRM);
    sigprocmask(SIG_BLOCK, &a, NULL);
    CHECK(timer_create(CLOCK_MONOTONIC, NULL, &t) == 0, "default sigevent");
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    now.tv_nsec += 30000000; if (now.tv_nsec >= 1000000000) { now.tv_sec++; now.tv_nsec -= 1000000000; }
    it = (struct itimerspec){ .it_value = now };
    CHECK(timer_settime(t, TIMER_ABSTIME, &it, NULL) == 0, "abstime");
    g = sigtimedwait(&a, &si, &ts1s);
    CHECK(g == SIGALRM && si.si_code == SI_TIMER, "abs timer %d", g);
    timer_delete(t);
    sigprocmask(SIG_UNBLOCK, &a, NULL);
    ev = (struct sigevent){ .sigev_notify = SIGEV_THREAD, .sigev_notify_function = thr_cb, .sigev_value.sival_int = 7 };
    CHECK(timer_create(CLOCK_MONOTONIC, &ev, &t) == 0, "SIGEV_THREAD");
    it = (struct itimerspec){ .it_value = { 0, 10000000 }, .it_interval = { 0, 10000000 } };
    timer_settime(t, 0, &it, NULL);
    for (int i = 0; i < 100 && thr_calls < 3; i++) usleep(10000);
    CHECK(thr_calls >= 3 && thr_val == 7, "SIGEV_THREAD calls %d", thr_calls);
    timer_delete(t);
    ev = (struct sigevent){ .sigev_notify = SIGEV_SIGNAL, .sigev_signo = 999 };
    CHECK(timer_create(CLOCK_MONOTONIC, &ev, &t) < 0 && errno == EINVAL, "bad signo");
    ev = (struct sigevent){ .sigev_notify = SIGEV_THREAD_ID, .sigev_signo = rs };
    ev.sigev_notify_thread_id = 999999;
    int kid;
    CHECK(syscall(SYS_timer_create, CLOCK_MONOTONIC, &ev, &kid) < 0 && errno == EINVAL, "bad thread id");
    CHECK(timer_create(12345, NULL, &t) < 0 && errno == EINVAL, "bad clock");
    sigprocmask(SIG_UNBLOCK, &s, NULL);
}

/* ---- robust futexes ---- */
static pthread_mutex_t *rm;
static void *locker(void *a) { (void)a; pthread_mutex_lock(rm); return NULL; }   /* exits holding it */

static void test_robust(void) {
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_setrobust(&at, PTHREAD_MUTEX_ROBUST);
    pthread_mutexattr_setpshared(&at, PTHREAD_PROCESS_SHARED);
    rm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    pthread_mutex_init(rm, &at);
    pthread_t th;
    pthread_create(&th, NULL, locker, NULL);
    pthread_join(th, NULL);
    int r = pthread_mutex_lock(rm);
    CHECK(r == EOWNERDEAD, "thread owner died: %d", r);
    CHECK(pthread_mutex_consistent(rm) == 0, "consistent");
    pthread_mutex_unlock(rm);
    fprintf(stderr, "sigqtest: robust thread done\n");
    /* another process dies holding it while we block on it */
    pid_t c = fork();
    if (!c) { pthread_mutex_lock(rm); usleep(100000); _exit(0); }
    usleep(30000);
    r = pthread_mutex_lock(rm);
    CHECK(r == EOWNERDEAD, "process owner died: %d", r);
    pthread_mutex_consistent(rm);
    pthread_mutex_unlock(rm);
    waitpid(c, NULL, 0);
    void *head = NULL; size_t len = 0;
    CHECK(syscall(SYS_get_robust_list, 0, &head, &len) == 0 && head && len == 24, "get_robust_list %p %zu", head, len);
    CHECK(syscall(SYS_set_robust_list, head, 23) < 0 && errno == EINVAL, "set_robust_list bad len");
    munmap(rm, 4096);
}

/* ---- clone3, pidfds, waitid ---- */
struct clone_args_ { uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls, set_tid, set_tid_size, cgroup; };

static void test_pidfd(void) {
    int pidfd = -1;
    struct clone_args_ ca = { .flags = CLONE_PIDFD, .pidfd = (uintptr_t)&pidfd, .exit_signal = SIGCHLD };
    long c = syscall(SYS_clone3, &ca, sizeof ca);
    if (c == 0) { usleep(50000); _exit(7); }
    CHECK(c > 0 && pidfd >= 0, "clone3 %ld pidfd %d", c, pidfd);
    CHECK(fcntl(pidfd, F_GETFD) & FD_CLOEXEC, "pidfd is close-on-exec");
    struct pollfd pf = { pidfd, POLLIN, 0 };
    CHECK(poll(&pf, 1, 0) == 0, "pidfd not ready before exit");
    CHECK(poll(&pf, 1, 2000) == 1 && (pf.revents & POLLIN), "pidfd ready at exit");
    siginfo_t si; memset(&si, 0, sizeof si);
    CHECK(waitid(P_PIDFD, pidfd, &si, WEXITED | WNOWAIT) == 0 && si.si_pid == c && si.si_status == 7, "WNOWAIT");
    memset(&si, 0, sizeof si);
    CHECK(waitid(P_PIDFD, pidfd, &si, WEXITED) == 0 && si.si_pid == c && si.si_code == CLD_EXITED && si.si_status == 7, "waitid P_PIDFD");
    errno = 0;
    CHECK(waitid(P_PIDFD, pidfd, &si, WEXITED) < 0 && errno == ECHILD, "reaped");
    close(pidfd);
    ca = (struct clone_args_){ .exit_signal = SIGCHLD };
    CHECK(syscall(SYS_clone3, &ca, 32) < 0 && errno == EINVAL, "short size");
    ca.flags = SIGCHLD;
    CHECK(syscall(SYS_clone3, &ca, sizeof ca) < 0 && errno == EINVAL, "CSIGNAL in flags");
    pid_t k = fork();
    if (!k) { for (;;) pause(); }
    int fd = syscall(SYS_pidfd_open, k, 0);
    CHECK(fd >= 0, "pidfd_open");
    memset(&si, 0, sizeof si);
    CHECK(waitid(P_PID, k, &si, WEXITED | WNOHANG) == 0 && si.si_pid == 0, "WNOHANG not exited");
    CHECK(syscall(SYS_pidfd_send_signal, fd, SIGSTOP, NULL, 0) == 0, "send SIGSTOP");
    CHECK(waitid(P_PID, k, &si, WSTOPPED) == 0 && si.si_code == CLD_STOPPED && si.si_status == SIGSTOP, "WSTOPPED code %d", si.si_code);
    kill(k, SIGCONT);
    CHECK(waitid(P_PGID, getpgrp(), &si, WCONTINUED) == 0 && si.si_pid == k && si.si_code == CLD_CONTINUED, "WCONTINUED code %d", si.si_code);
    CHECK(syscall(SYS_pidfd_send_signal, fd, SIGTERM, NULL, 0) == 0, "send SIGTERM");
    CHECK(waitid(P_ALL, 0, &si, WEXITED) == 0 && si.si_pid == k && si.si_code == CLD_KILLED && si.si_status == SIGTERM, "killed code %d", si.si_code);
    errno = 0;
    CHECK(syscall(SYS_pidfd_send_signal, fd, SIGTERM, NULL, 0) < 0 && errno == ESRCH, "signal to dead process");
    close(fd);
    CHECK(syscall(SYS_pidfd_open, 999999, 0) < 0 && errno == ESRCH, "pidfd_open ESRCH");
    int pp[2]; pipe(pp);
    k = fork();
    if (!k) {
        int f = open("/proc/self/status", O_RDONLY);
        char n = (char)f;
        write(pp[1], &n, 1);
        pause();
        _exit(0);
    }
    char n; read(pp[0], &n, 1);
    fd = syscall(SYS_pidfd_open, k, 0);
    int got = syscall(SYS_pidfd_getfd, fd, n, 0);
    CHECK(got >= 0 && (fcntl(got, F_GETFD) & FD_CLOEXEC), "pidfd_getfd %d", got);
    char buf[256] = { 0 };
    if (got >= 0) { read(got, buf, sizeof buf - 1); close(got); }
    char want[32]; snprintf(want, sizeof want, "Pid:\t%d", k);
    CHECK(strstr(buf, want) != NULL, "fd is the child's /proc/self/status");
    CHECK(syscall(SYS_pidfd_getfd, fd, 200, 0) < 0 && errno == EBADF, "getfd EBADF");
    syscall(SYS_pidfd_send_signal, fd, SIGKILL, NULL, 0);
    waitpid(k, NULL, 0);
    close(fd); close(pp[0]); close(pp[1]);
}

int main(void) {
    test_queue();  fprintf(stderr, "sigqtest: queue done\n");
    test_timers(); fprintf(stderr, "sigqtest: timers done\n");
    test_robust(); fprintf(stderr, "sigqtest: robust done\n");
    test_pidfd();  fprintf(stderr, "sigqtest: pidfd done\n");
    if (fails) { printf("sigqtest: %d failures\n", fails); return 1; }
    printf("sigqtest: all passed\n");
    return 0;
}
