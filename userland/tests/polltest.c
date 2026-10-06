/* poll/select/ppoll/pselect/epoll without the BKL: cross-thread wakeups on pipes, eventfd and
 * AF_UNIX, poll racing close/dup2 of the polled fds, ppoll/pselect signal masks (EINTR + mask
 * restore), mixed lock-free and BKL (tty, timerfd) files in one set, epoll wakeups and
 * concurrent epoll_ctl. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("polltest: FAIL " __VA_ARGS__); putchar('\n'); fails++; } } while (0)

#define ROUNDS 300
static int pfd[2], sv[2], efd;
static volatile int stop, pdone;

static void *pinger(void *a) {
    (void)a;
    for (int i = 0; i < ROUNDS; i++) {
        uint64_t one = 1; char c = 'x';
        switch (i % 3) {
        case 0: write(pfd[1], &c, 1); break;
        case 1: write(sv[1], &c, 1); break;
        case 2: write(efd, &one, 8); break;
        }
        usleep(200);
    }
    pdone = 1;
    return 0;
}

static void drain(int fd, int isefd) {
    char buf[64]; uint64_t v;
    if (isefd) read(fd, &v, 8); else read(fd, buf, sizeof buf);
}

static void test_wakeups(void) {
    pipe2(pfd, O_NONBLOCK);
    socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv);
    efd = eventfd(0, EFD_NONBLOCK);
    pdone = 0; pthread_t t; pthread_create(&t, 0, pinger, 0);
    int got = 0, timeouts = 0;
    while (got < ROUNDS && timeouts < 20) {
        struct pollfd p[3] = { { pfd[0], POLLIN, 0 }, { sv[0], POLLIN, 0 }, { efd, POLLIN, 0 } };
        int n = poll(p, 3, 1000);
        if (n == 0) { if (pdone) break; timeouts++; continue; }
        for (int i = 0; i < 3; i++) if (p[i].revents & POLLIN) { drain(p[i].fd, i == 2); got++; }
    }
    pthread_join(t, 0);
    /* the writer may coalesce: count >= ROUNDS/3 per source is not guaranteed, just no stalls */
    CHECK(timeouts == 0, "poll wakeups stalled (%d timeouts, %d events)", timeouts, got);

    /* select on the same set, woken from another thread */
    pdone = 0; pthread_create(&t, 0, pinger, 0);
    got = 0; timeouts = 0;
    while (got < ROUNDS / 2 && timeouts < 20) {
        fd_set r; FD_ZERO(&r); FD_SET(pfd[0], &r); FD_SET(sv[0], &r); FD_SET(efd, &r);
        struct timeval tv = { 1, 0 };
        int mx = pfd[0] > sv[0] ? pfd[0] : sv[0]; if (efd > mx) mx = efd;
        int n = select(mx + 1, &r, 0, 0, &tv);
        if (n == 0) { if (pdone) break; timeouts++; continue; }
        if (FD_ISSET(pfd[0], &r)) { drain(pfd[0], 0); got++; }
        if (FD_ISSET(sv[0], &r)) { drain(sv[0], 0); got++; }
        if (FD_ISSET(efd, &r)) { drain(efd, 1); got++; }
    }
    pthread_join(t, 0);
    CHECK(timeouts == 0, "select wakeups stalled (%d timeouts)", timeouts);
}

/* another thread keeps replacing the polled fd: poll must never crash or see a freed file */
static int racefd = -1;
static void *closer(void *a) {
    (void)a;
    while (!stop) {
        int p[2]; pipe(p);
        dup2(p[0], racefd);
        close(p[0]); close(p[1]);          /* write end closed: POLLHUP on racefd */
        int e = eventfd(1, 0); dup2(e, racefd); close(e);
    }
    return 0;
}
static void test_close_race(void) {
    racefd = eventfd(0, 0);
    stop = 0;
    pthread_t t[2]; pthread_create(&t[0], 0, closer, 0); pthread_create(&t[1], 0, closer, 0);
    int bad = 0;
    for (int i = 0; i < 3000; i++) {
        struct pollfd p = { racefd, POLLIN, 0 };
        int n = poll(&p, 1, 10);
        if (n < 0 || (n == 1 && !(p.revents & (POLLIN | POLLHUP)))) bad++;
    }
    stop = 1;
    pthread_join(t[0], 0); pthread_join(t[1], 0);
    CHECK(!bad, "poll vs close/dup2 race: %d bad results", bad);
    close(racefd);
}

static volatile int hits;
static void on_usr1(int s) { (void)s; hits++; }
static pthread_t main_thr;
static void *kicker(void *a) { (void)a; usleep(20000); pthread_kill(main_thr, SIGUSR1); return 0; }

static void test_sigmask(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, 0);
    sigset_t blk, empty, cur; sigemptyset(&blk); sigaddset(&blk, SIGUSR1); sigemptyset(&empty);
    sigprocmask(SIG_BLOCK, &blk, 0);
    main_thr = pthread_self();
    int p[2]; pipe(p);
    struct pollfd pf = { p[0], POLLIN, 0 };
    pthread_t t; pthread_create(&t, 0, kicker, 0);
    struct timespec ts = { 2, 0 };
    int r = ppoll(&pf, 1, &ts, &empty);
    int e = errno;
    pthread_join(t, 0);
    CHECK(r == -1 && e == EINTR, "ppoll EINTR r=%d errno=%d", r, e);
    CHECK(hits == 1, "ppoll handler ran %d times", hits);
    sigprocmask(SIG_BLOCK, 0, &cur);
    CHECK(sigismember(&cur, SIGUSR1), "ppoll restored mask");

    /* blocked signal does not interrupt a plain timed poll */
    pthread_create(&t, 0, kicker, 0);
    r = poll(&pf, 1, 60);
    pthread_join(t, 0);
    CHECK(r == 0 && hits == 1, "poll with SIGUSR1 blocked r=%d hits=%d", r, hits);
    /* now pending: pselect with an empty mask delivers it at once */
    fd_set rs; FD_ZERO(&rs); FD_SET(p[0], &rs);
    r = pselect(p[0] + 1, &rs, 0, 0, &ts, &empty);
    e = errno;
    CHECK(r == -1 && e == EINTR && hits == 2, "pselect pending EINTR r=%d errno=%d hits=%d", r, e, hits);
    sigprocmask(SIG_BLOCK, 0, &cur);
    CHECK(sigismember(&cur, SIGUSR1), "pselect restored mask");
    sigprocmask(SIG_UNBLOCK, &blk, 0);
    close(p[0]); close(p[1]);
}

static void test_mixed(void) {
    /* lock-free pipe + BKL files (tty, timerfd) in one set */
    int p[2]; pipe(p);
    int tf = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec its = { { 0, 0 }, { 0, 30 * 1000000 } };
    timerfd_settime(tf, 0, &its, 0);
    struct pollfd pf[3] = { { p[0], POLLIN, 0 }, { tf, POLLIN, 0 }, { 1, POLLOUT, 0 } };
    int n = poll(pf, 2, 2000);
    CHECK(n == 1 && (pf[1].revents & POLLIN), "timerfd wakeup n=%d", n);
    n = poll(pf, 3, 0);
    CHECK(n >= 2 && (pf[2].revents & POLLOUT), "tty POLLOUT n=%d", n);
    struct pollfd bad = { 999, POLLIN, 0 };
    CHECK(poll(&bad, 1, 0) == 1 && bad.revents == POLLNVAL, "POLLNVAL");
    close(p[0]); close(p[1]); close(tf);
}

/* epoll: wakeups from another thread, plus concurrent epoll_ctl add/del on the same instance */
static int ep;
static void *ctl_churn(void *a) {
    (void)a;
    while (!stop) {
        int e = eventfd(0, 0);
        struct epoll_event ev = { EPOLLIN, { .fd = e } };
        epoll_ctl(ep, EPOLL_CTL_ADD, e, &ev);
        epoll_ctl(ep, EPOLL_CTL_DEL, e, 0);
        close(e);
    }
    return 0;
}
static void test_epoll(void) {
    pipe2(pfd, O_NONBLOCK);
    socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv);
    efd = eventfd(0, EFD_NONBLOCK);
    ep = epoll_create1(0);
    int fds[3] = { pfd[0], sv[0], efd };
    for (int i = 0; i < 3; i++) {
        struct epoll_event ev = { EPOLLIN, { .u32 = i } };
        CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fds[i], &ev) == 0, "epoll add %d", i);
    }
    stop = 0;
    pthread_t c; pthread_create(&c, 0, ctl_churn, 0);
    pdone = 0; pdone = 0; pthread_t t; pthread_create(&t, 0, pinger, 0);
    int got = 0, timeouts = 0;
    while (got < ROUNDS && timeouts < 20) {
        struct epoll_event evs[8];
        int n = epoll_wait(ep, evs, 8, 1000);
        if (n == 0) { if (pdone) break; timeouts++; continue; }
        for (int i = 0; i < n; i++) if (evs[i].data.u32 < 3) { drain(fds[evs[i].data.u32], evs[i].data.u32 == 2); got++; }
    }
    pthread_join(t, 0);
    stop = 1;
    pthread_join(c, 0);
    CHECK(timeouts == 0, "epoll wakeups stalled (%d timeouts, %d events)", timeouts, got);
    /* closing a registered fd drops it from the set */
    close(efd);
    uint64_t one = 1; (void)one;
    struct epoll_event evs[4];
    CHECK(epoll_wait(ep, evs, 4, 0) == 0, "closed fd not reported");
    /* nested instances: inner readiness shows through the outer one; loops are refused */
    int inner = epoll_create1(0), e2 = eventfd(0, 0);
    struct epoll_event iev = { EPOLLIN, { .fd = e2 } };
    CHECK(epoll_ctl(inner, EPOLL_CTL_ADD, e2, &iev) == 0, "inner add");
    struct epoll_event oev = { EPOLLIN, { .u32 = 77 } };
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, inner, &oev) == 0, "nest add");
    CHECK(epoll_wait(ep, evs, 4, 0) == 0, "nested idle");
    uint64_t v1 = 1; write(e2, &v1, 8);
    CHECK(epoll_wait(ep, evs, 4, 100) == 1 && evs[0].data.u32 == 77, "nested ready");
    struct epoll_event lev = { EPOLLIN, { 0 } };
    CHECK(epoll_ctl(inner, EPOLL_CTL_ADD, ep, &lev) == -1 && errno == ELOOP, "ELOOP");
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, ep, &lev) == -1 && errno == EINVAL, "self add EINVAL");
    struct pollfd pp = { ep, POLLIN, 0 };
    CHECK(poll(&pp, 1, 0) == 1, "poll on epoll fd");
    close(inner); close(e2);
    close(ep); close(pfd[0]); close(pfd[1]); close(sv[0]); close(sv[1]);
}

int main(void) {
    test_wakeups();
    close(pfd[0]); close(pfd[1]); close(sv[0]); close(sv[1]); close(efd);
    test_close_race();
    test_sigmask();
    test_mixed();
    test_epoll();
    if (fails) return 1;
    puts("polltest: OK");
    return 0;
}
