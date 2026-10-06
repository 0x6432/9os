/* AF_UNIX without the BKL: concurrent SEQPACKET senders/receivers (record atomicity), a
 * cross-process stream bulk copy, SCM_RIGHTS churn, send-vs-peer-close races, accept/connect
 * churn on an abstract listener, and DGRAM fan-in with recvfrom. */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("socktest: FAIL " __VA_ARGS__); putchar('\n'); fails++; } } while (0)

#define REC 96
#define PER 3000
static int sp[2];
static volatile long got, bad;
static void *sender(void *a) {
    unsigned char r[REC];
    for (int i = 0; i < PER; i++) {
        unsigned char s = 0;
        for (int k = 1; k < REC; k++) { r[k] = (unsigned char)((long)a * 77 + i * 3 + k); s += r[k]; }
        r[0] = (unsigned char)-s;
        if (send(sp[0], r, REC, MSG_NOSIGNAL) != REC) __atomic_add_fetch(&bad, 1, __ATOMIC_RELAXED);
    }
    return 0;
}
static void *receiver(void *a) {
    unsigned char b[REC * 2];
    for (;;) {
        ssize_t n = recv(sp[1], b, sizeof b, 0);
        if (n <= 0) break;
        unsigned char s = 0;
        for (ssize_t k = 0; k < n; k++) s += b[k];
        if (n != REC || s) __atomic_add_fetch(&bad, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&got, 1, __ATOMIC_RELAXED);
    }
    return 0;
}


static struct sockaddr_un abstract(const char *n, socklen_t *len) {
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    a.sun_path[0] = 0; strcpy(a.sun_path + 1, n);
    *len = (socklen_t)(sizeof(sa_family_t) + 1 + strlen(n));
    return a;
}

#define NCLI 3
#define CONNS 60
static int lfd;
static void *client(void *arg) {
    socklen_t len; struct sockaddr_un a = abstract("9os-socktest", &len);
    for (int i = 0; i < CONNS; i++) {
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        while (connect(c, (struct sockaddr *)&a, len) < 0) {
            if (errno != EAGAIN && errno != ECONNREFUSED) { __atomic_add_fetch(&bad, 1, __ATOMIC_RELAXED); break; }
            usleep(500);
        }
        uint32_t v = (uint32_t)((long)arg * 1000 + i), back = 0;
        if (write(c, &v, 4) != 4 || read(c, &back, 4) != 4 || back != v + 1) __atomic_add_fetch(&bad, 1, __ATOMIC_RELAXED);
        close(c);
    }
    return 0;
}

static int dg;
static void *dgclient(void *arg) {
    socklen_t len; struct sockaddr_un a = abstract("9os-socktest-dg", &len);
    int c = socket(AF_UNIX, SOCK_DGRAM, 0);
    for (int i = 0; i < 1500; i++) {
        uint32_t v = (uint32_t)i;
        while (sendto(c, &v, 4, 0, (struct sockaddr *)&a, len) < 0) {
            if (errno != EAGAIN) { __atomic_add_fetch(&bad, 1, __ATOMIC_RELAXED); break; }
            usleep(200);
        }
    }
    close(c);
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    /* 1: SEQPACKET MPMC */
    socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sp);
    pthread_t s[2], r[2];
    for (long i = 0; i < 2; i++) pthread_create(&r[i], 0, receiver, 0);
    for (long i = 0; i < 2; i++) pthread_create(&s[i], 0, sender, (void *)i);
    for (int i = 0; i < 2; i++) pthread_join(s[i], 0);
    shutdown(sp[0], SHUT_WR);
    for (int i = 0; i < 2; i++) pthread_join(r[i], 0);
    CHECK(!bad && got == 2 * PER, "seqpacket bad=%ld got=%ld", bad, got);
    close(sp[0]); close(sp[1]);
    fprintf(stderr, "socktest: seqpacket done\n");

    /* 2: stream bulk across processes */
    int st[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, st);
    pid_t pid = fork();
    if (!pid) {
        close(st[0]);
        unsigned char b[7000];
        for (int i = 0; i < 400; i++) { memset(b, i & 0xff, sizeof b); if (write(st[1], b, sizeof b) != (ssize_t)sizeof b) _exit(1); }
        _exit(0);
    }
    close(st[1]);
    uint64_t sum = 0, total = 0; unsigned char b[5000]; ssize_t n;
    while ((n = read(st[0], b, sizeof b)) > 0) { for (ssize_t i = 0; i < n; i++) sum += b[i]; total += n; }
    int ws; waitpid(pid, &ws, 0);
    uint64_t want = 0; for (int i = 0; i < 400; i++) want += (uint64_t)(i & 0xff) * 7000;
    CHECK(total == 400 * 7000 && sum == want && WIFEXITED(ws) && !WEXITSTATUS(ws), "bulk total=%lu", (unsigned long)total);
    close(st[0]);
    fprintf(stderr, "socktest: bulk done\n");

    /* 3: SCM_RIGHTS churn */
    int rp[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, rp);
    for (int i = 0; i < 200; i++) {
        int p[2]; pipe(p);
        char c = 'a' + i % 26;
        struct iovec iov = { &c, 1 };
        char cb[CMSG_SPACE(sizeof(int))] = {0};
        struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb };
        struct cmsghdr *h = CMSG_FIRSTHDR(&m);
        h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(h), &p[1], sizeof(int));
        if (sendmsg(rp[0], &m, 0) != 1) { fails++; break; }
        close(p[1]);
        char rc = 0; char rcb[CMSG_SPACE(sizeof(int))];
        struct iovec riov = { &rc, 1 };
        struct msghdr rm = { .msg_iov = &riov, .msg_iovlen = 1, .msg_control = rcb, .msg_controllen = sizeof rcb };
        if (recvmsg(rp[1], &rm, 0) != 1 || rc != c) { fails++; break; }
        struct cmsghdr *rh = CMSG_FIRSTHDR(&rm);
        int wfd = -1; if (rh && rh->cmsg_type == SCM_RIGHTS) memcpy(&wfd, CMSG_DATA(rh), sizeof(int));
        char x = 'z', y = 0;
        if (wfd < 0 || write(wfd, &x, 1) != 1 || read(p[0], &y, 1) != 1 || y != 'z') { CHECK(0, "rights iteration %d", i); break; }
        close(wfd); close(p[0]);
    }
    close(rp[0]); close(rp[1]);
    fprintf(stderr, "socktest: rights done\n");

    /* 4: send while the peer closes */
    for (int i = 0; i < 150; i++) {
        int q[2]; socketpair(AF_UNIX, i & 1 ? SOCK_STREAM : SOCK_SEQPACKET, 0, q);
        pid_t k = fork();
        if (!k) { usleep(i % 7 * 100); close(q[1]); _exit(0); }
        close(q[1]);
        char buf[512] = {0};
        int sawpipe = 0;
        for (int j = 0; j < 400 && !sawpipe; j++) {
            ssize_t w = send(q[0], buf, sizeof buf, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (w < 0 && errno == EPIPE) sawpipe = 1;
            else if (w < 0 && errno != EAGAIN) { CHECK(0, "close race errno %d", errno); break; }
            else if (w < 0) usleep(100);
        }
        waitpid(k, 0, 0);
        if (!sawpipe) {   /* the child's copy is closed now: the next send must fail */
            ssize_t w = send(q[0], buf, 1, MSG_NOSIGNAL);
            CHECK(w < 0 && errno == EPIPE, "no EPIPE after close (w=%zd errno=%d)", w, errno);
        }
        close(q[0]);
    }
    fprintf(stderr, "socktest: close-race done\n");

    /* 5: accept/connect churn */
    socklen_t len; struct sockaddr_un a = abstract("9os-socktest", &len);
    lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(!bind(lfd, (struct sockaddr *)&a, len) && !listen(lfd, 4), "bind/listen");
    bad = 0;
    pthread_t cl[NCLI];
    for (long i = 0; i < NCLI; i++) pthread_create(&cl[i], 0, client, (void *)i);
    for (int i = 0; i < NCLI * CONNS; i++) {
        int c = accept(lfd, 0, 0);
        uint32_t v;
        if (c < 0 || read(c, &v, 4) != 4) { bad++; if (c >= 0) close(c); continue; }
        v++; write(c, &v, 4); close(c);
    }
    for (int i = 0; i < NCLI; i++) pthread_join(cl[i], 0);
    CHECK(!bad, "accept churn bad=%ld", bad);
    close(lfd);
    fprintf(stderr, "socktest: accept done\n");

    /* 6: datagram fan-in */
    struct sockaddr_un da = abstract("9os-socktest-dg", &len);
    dg = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(!bind(dg, (struct sockaddr *)&da, len), "dgram bind");
    bad = 0;
    pthread_t dc[2];
    for (long i = 0; i < 2; i++) pthread_create(&dc[i], 0, dgclient, (void *)i);
    long msgs = 0;
    while (msgs < 3000) {
        struct pollfd p = { dg, POLLIN, 0 };
        if (poll(&p, 1, 5000) <= 0) break;
        uint32_t v; struct sockaddr_un from; socklen_t fl = sizeof from;
        if (recvfrom(dg, &v, 4, 0, (struct sockaddr *)&from, &fl) == 4) msgs++;
    }
    for (int i = 0; i < 2; i++) pthread_join(dc[i], 0);
    CHECK(!bad && msgs == 3000, "dgram msgs=%ld bad=%ld", msgs, bad);
    close(dg);

    if (fails) return 1;
    puts("socktest: OK");
    return 0;
}
