/* ipctest: eventfd, timerfd, signalfd, epoll (incl. nesting), memfd + MAP_SHARED, AF_UNIX
 * stream/dgram/seqpacket sockets, SCM_RIGHTS fd passing and SO_PEERCRED. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("  FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno); fails++; } } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static int verbose;
#define STEP(name) do { if (verbose) printf("  [%s]\n", name); } while (0)
static void test_eventfd(void) {
    int e = eventfd(3, EFD_NONBLOCK | EFD_CLOEXEC);
    CHECK(e >= 0);
    uint64_t v = 0;
    CHECK(read(e, &v, 8) == 8 && v == 3);
    CHECK(read(e, &v, 8) < 0 && errno == EAGAIN);
    v = 5; CHECK(write(e, &v, 8) == 8);
    v = 2; CHECK(write(e, &v, 8) == 8);
    CHECK(read(e, &v, 8) == 8 && v == 7);
    close(e);
    int s = eventfd(2, EFD_SEMAPHORE);
    CHECK(read(s, &v, 8) == 8 && v == 1 && read(s, &v, 8) == 8 && v == 1);
    close(s);
    printf("eventfd ok\n");
}

static void test_timerfd(void) {
    int t = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    CHECK(t >= 0);
    struct itimerspec its = { { 0, 20000000 }, { 0, 30000000 } };   /* 30 ms, then every 20 ms */
    double t0 = now();
    CHECK(timerfd_settime(t, 0, &its, NULL) == 0);
    uint64_t n = 0;
    CHECK(read(t, &n, 8) == 8 && n >= 1);
    double dt = now() - t0;
    CHECK(dt > 0.025 && dt < 0.2);
    usleep(70000);
    CHECK(read(t, &n, 8) == 8 && n >= 3);
    struct itimerspec cur;
    CHECK(timerfd_gettime(t, &cur) == 0 && cur.it_interval.tv_nsec == 20000000);
    struct itimerspec off = {0};
    CHECK(timerfd_settime(t, 0, &off, NULL) == 0);
    close(t);
    printf("timerfd ok (first expiry after %.1f ms)\n", dt * 1e3);
}

static void test_signalfd(void) {
    sigset_t m; sigemptyset(&m); sigaddset(&m, SIGUSR1); sigaddset(&m, SIGCHLD);
    sigprocmask(SIG_BLOCK, &m, NULL);
    int s = signalfd(-1, &m, SFD_NONBLOCK);
    CHECK(s >= 0);
    struct signalfd_siginfo si;
    CHECK(read(s, &si, sizeof si) < 0 && errno == EAGAIN);
    kill(getpid(), SIGUSR1);
    CHECK(read(s, &si, sizeof si) == sizeof si && si.ssi_signo == SIGUSR1);
    pid_t c = fork(); if (!c) _exit(0);
    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.u32 = 7 };
    epoll_ctl(ep, EPOLL_CTL_ADD, s, &ev);
    struct epoll_event out;
    CHECK(epoll_wait(ep, &out, 1, 2000) == 1 && out.data.u32 == 7);
    CHECK(read(s, &si, sizeof si) == sizeof si && si.ssi_signo == SIGCHLD);
    waitpid(c, NULL, 0);
    close(ep); close(s);
    sigprocmask(SIG_UNBLOCK, &m, NULL);
    printf("signalfd ok\n");
}

static void test_epoll(void) {
    int ep = epoll_create1(EPOLL_CLOEXEC);
    CHECK(ep >= 0);
    int p[2]; pipe(p);
    int e = eventfd(0, EFD_NONBLOCK);
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0x1111 };
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) < 0 && errno == EEXIST);
    ev.data.u64 = 0x2222; ev.events = EPOLLIN | EPOLLET;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, e, &ev) == 0);
    struct epoll_event out[4];
    CHECK(epoll_wait(ep, out, 4, 0) == 0);
    double t0 = now();
    CHECK(epoll_wait(ep, out, 4, 50) == 0 && now() - t0 > 0.04);
    write(p[1], "x", 1);
    CHECK(epoll_wait(ep, out, 4, 1000) == 1 && out[0].data.u64 == 0x1111 && (out[0].events & EPOLLIN));
    uint64_t one = 1; write(e, &one, 8);
    int n = epoll_wait(ep, out, 4, 1000);
    CHECK(n == 2);
    /* nested epoll: the inner set is readable */
    int outer = epoll_create1(0);
    ev.events = EPOLLIN; ev.data.u64 = 0x3333;
    CHECK(epoll_ctl(outer, EPOLL_CTL_ADD, ep, &ev) == 0);
    CHECK(epoll_wait(outer, out, 4, 1000) == 1 && out[0].data.u64 == 0x3333);
    /* wake-up from another process */
    char c; read(p[0], &c, 1); read(e, &one, 8);
    pid_t k = fork();
    if (!k) { usleep(30000); write(p[1], "y", 1); _exit(0); }
    t0 = now();
    CHECK(epoll_wait(ep, out, 4, 3000) == 1 && out[0].data.u64 == 0x1111);
    CHECK(now() - t0 > 0.02);
    waitpid(k, NULL, 0);
    /* DEL and closing removes items */
    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL) == 0);
    CHECK(epoll_wait(ep, out, 4, 0) == 0);
    /* oneshot */
    ev.events = EPOLLIN | EPOLLONESHOT; ev.data.u64 = 5;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
    CHECK(epoll_wait(ep, out, 4, 0) == 1 && epoll_wait(ep, out, 4, 0) == 0);
    close(p[0]); close(p[1]); close(e); close(outer); close(ep);
    printf("epoll ok\n");
}

static void test_memfd(void) {
    int m = memfd_create("pool", MFD_CLOEXEC);
    CHECK(m >= 0);
    CHECK(ftruncate(m, 3 * 4096) == 0);
    struct stat st; CHECK(fstat(m, &st) == 0 && st.st_size == 3 * 4096);
    char *a = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, m, 0);
    CHECK(a != MAP_FAILED);
    strcpy(a + 4096, "shared!");
    char buf[8] = {0};
    CHECK(pread(m, buf, 7, 4096) == 7 && !strcmp(buf, "shared!"));
    pwrite(m, "PW", 2, 0);
    CHECK(a[0] == 'P' && a[1] == 'W');
    char *b = mmap(NULL, 4096, PROT_READ, MAP_SHARED, m, 4096);          /* second mapping, offset */
    CHECK(b != MAP_FAILED && !strcmp(b, "shared!"));
    pid_t c = fork();
    if (!c) { strcpy(a + 8192, "child"); _exit(0); }
    waitpid(c, NULL, 0);
    CHECK(!strcmp(a + 8192, "child"));
    munmap(b, 4096); munmap(a, 3 * 4096); close(m);
    printf("memfd ok\n");
}

static int send_fd(int s, int fd, const char *msg) {
    struct iovec iov = { (void *)msg, strlen(msg) };
    char ctl[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof ctl };
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    return sendmsg(s, &mh, MSG_NOSIGNAL);
}
static int recv_fd(int s, char *buf, size_t n, int *fd) {
    struct iovec iov = { buf, n };
    char ctl[CMSG_SPACE(sizeof(int) * 4)];
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof ctl };
    int r = recvmsg(s, &mh, MSG_CMSG_CLOEXEC);
    *fd = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(fd, CMSG_DATA(c), sizeof(int));
    return r;
}

static void test_unix(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
    CHECK(write(sv[0], "hello", 5) == 5);
    char buf[64] = {0};
    CHECK(read(sv[1], buf, sizeof buf) == 5 && !memcmp(buf, "hello", 5));
    STEP("pair");
    /* pass a pipe's write end and use it */
    int p[2]; pipe(p);
    CHECK(send_fd(sv[0], p[1], "fd") == 2);
    int got;
    signal(SIGPIPE, SIG_IGN);
    int rr = recv_fd(sv[1], buf, sizeof buf, &got);
    if (verbose) printf("  recv_fd=%d got=%d errno=%d\n", rr, got, errno);
    CHECK(rr == 2 && got >= 0 && got != p[1]);
    CHECK(write(got, "via", 3) == 3);
    CHECK(read(p[0], buf, 3) == 3 && !memcmp(buf, "via", 3));
    CHECK(fcntl(got, F_GETFD) & FD_CLOEXEC);
    close(got); close(p[0]); close(p[1]);
    struct ucred cr; socklen_t cl = sizeof cr;
    CHECK(getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED, &cr, &cl) == 0 && cr.pid == getpid());
    close(sv[1]);
    CHECK(read(sv[0], buf, 1) == 0);                             /* EOF */
    CHECK(write(sv[0], "x", 1) < 0 && errno == EPIPE);
    close(sv[0]);

    STEP("seqpacket");
    /* seqpacket keeps message boundaries */
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
    write(sv[0], "abc", 3); write(sv[0], "defg", 4);
    CHECK(read(sv[1], buf, 64) == 3 && read(sv[1], buf, 64) == 4);
    close(sv[0]); close(sv[1]);

    STEP("listen");
    /* filesystem socket: listen/connect/accept across processes */
    const char *path = "/tmp/ipctest.sock";
    unlink(path);
    int l = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strcpy(a.sun_path, path);
    CHECK(bind(l, (struct sockaddr *)&a, sizeof a) == 0);
    struct stat st; CHECK(stat(path, &st) == 0 && S_ISSOCK(st.st_mode));
    CHECK(listen(l, 4) == 0);
    pid_t c = fork();
    if (!c) {
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(s, (struct sockaddr *)&a, sizeof a)) _exit(1);
        int m = memfd_create("x", 0);
        write(m, "memfd-data", 10);
        if (send_fd(s, m, "ping") != 4) _exit(2);
        char r[8] = {0};
        if (read(s, r, 4) != 4 || memcmp(r, "pong", 4)) _exit(3);
        _exit(0);
    }
    STEP("accept");
    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN }, out;
    epoll_ctl(ep, EPOLL_CTL_ADD, l, &ev);
    CHECK(epoll_wait(ep, &out, 1, 3000) == 1);
    struct sockaddr_un pa; socklen_t pl = sizeof pa;
    int s = accept4(l, (struct sockaddr *)&pa, &pl, SOCK_CLOEXEC);
    CHECK(s >= 0);
    int mfd = -1;
    memset(buf, 0, sizeof buf);
    CHECK(recv_fd(s, buf, 4, &mfd) == 4 && !memcmp(buf, "ping", 4) && mfd >= 0);
    char md[16] = {0};
    CHECK(pread(mfd, md, 10, 0) == 10 && !strcmp(md, "memfd-data"));
    CHECK(write(s, "pong", 4) == 4);
    int st2; waitpid(c, &st2, 0);
    CHECK(WIFEXITED(st2) && WEXITSTATUS(st2) == 0);
    struct sockaddr_un sn; socklen_t snl = sizeof sn;
    CHECK(getsockname(l, (struct sockaddr *)&sn, &snl) == 0 && !strcmp(sn.sun_path, path));
    close(mfd); close(s); close(l); close(ep); unlink(path);

    STEP("dgram");
    /* abstract datagram sockets */
    int d1 = socket(AF_UNIX, SOCK_DGRAM, 0), d2 = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un an = { .sun_family = AF_UNIX }; memcpy(an.sun_path, "\0ipct", 5);
    socklen_t al = offsetof(struct sockaddr_un, sun_path) + 5;
    CHECK(bind(d1, (struct sockaddr *)&an, al) == 0);
    CHECK(sendto(d2, "dg1", 3, 0, (struct sockaddr *)&an, al) == 3);
    CHECK(sendto(d2, "dg22", 4, 0, (struct sockaddr *)&an, al) == 4);
    CHECK(recv(d1, buf, 64, 0) == 3 && recv(d1, buf, 64, MSG_DONTWAIT) == 4);
    CHECK(recv(d1, buf, 64, MSG_DONTWAIT) < 0 && errno == EAGAIN);
    close(d1); close(d2);

    STEP("throughput");
    /* throughput */
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    pid_t w = fork();
    static char big[65536];
    if (!w) { close(sv[1]); for (int i = 0; i < 256; i++) write(sv[0], big, sizeof big); _exit(0); }
    close(sv[0]);
    double t0 = now(); long tot = 0; ssize_t r;
    while ((r = read(sv[1], big, sizeof big)) > 0) tot += r;
    double dt = now() - t0;
    waitpid(w, NULL, 0); close(sv[1]);
    CHECK(tot == 256L * 65536);
    printf("unix sockets ok (stream throughput %.0f MiB/s)\n", tot / dt / 1048576);
}

int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : "";
    verbose = getenv("V") != NULL;
    if (!*only || !strcmp(only, "eventfd")) test_eventfd();
    if (!*only || !strcmp(only, "timerfd")) test_timerfd();
    if (!*only || !strcmp(only, "signalfd")) test_signalfd();
    if (!*only || !strcmp(only, "epoll")) test_epoll();
    if (!*only || !strcmp(only, "memfd")) test_memfd();
    if (!*only || !strcmp(only, "unix")) test_unix();
    printf("ipctest: %s (%d checks, %d failures)\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails != 0;
}
