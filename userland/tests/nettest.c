/* M32 networking: TCP/UDP/ICMP over loopback (and, with -x HOST PORT, against the host test
 * server through QEMU's user network; -s PORT serves a TCP echo for host-driven tests).
 * usage: nettest            all loopback tests (run as root: some drop privileges in a child)
 *        nettest -x IP PORT external echo/HTTP checks against scripts/net-host-server.py
 *        nettest -s PORT    TCP echo server (forks into the background, serves N connections) */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
static int verbose;
#define CHECK(c, ...) do { if (verbose) { printf("  .. %s:%d ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } if (!(c)) { printf("nettest: FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf(" (errno %d %s)\n", errno, strerror(errno)); fails++; } } while (0)
#define OK(name) printf("nettest: %s ok\n", name)

static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static uint8_t pat(uint64_t i, unsigned seed) { uint64_t x = (i + 1) * 0x9e3779b97f4a7c15ull ^ seed; return (uint8_t)(x >> 31); }
static uint32_t rnd_state = 12345;
static uint32_t rnd(void) { rnd_state = rnd_state * 1103515245 + 12345; return rnd_state >> 8; }

static struct sockaddr_in sin_of(const char *ip, int port) {
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}
static int listener(const char *ip, int port, int backlog, int *outport) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = sin_of(ip, port);
    if (bind(s, (void *)&a, sizeof a) || listen(s, backlog)) { close(s); return -1; }
    socklen_t l = sizeof a;
    getsockname(s, (void *)&a, &l);
    if (outport) *outport = ntohs(a.sin_port);
    return s;
}
static int connect_to(const char *ip, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = sin_of(ip, port);
    if (connect(s, (void *)&a, sizeof a)) { int e = errno; close(s); errno = e; return -1; }
    return s;
}
static const char *stage;
static volatile size_t xfer;
static char stagebuf[64];
static ssize_t readn(int fd, void *b, size_t n) {
    size_t d = 0;
    while (d < n) { ssize_t r = read(fd, (char *)b + d, n - d); if (r < 0 && errno == EINTR) continue; if (r <= 0) return d ? (ssize_t)d : r; d += r; xfer += r; }
    return d;
}
static ssize_t writen(int fd, const void *b, size_t n) {
    size_t d = 0;
    while (d < n) { ssize_t r = write(fd, (const char *)b + d, n - d); if (r < 0 && errno == EINTR) continue; if (r <= 0) return d ? (ssize_t)d : r; d += r; xfer += r; }
    return d;
}

/* ---------------------------------------------------------------- TCP */
static void tcp_basic(void) {
    int port, l = listener("127.0.0.1", 0, 4, &port);
    CHECK(l >= 0 && port >= 32768 && port < 61000, "listen on an ephemeral port (%d)", port);
    int c = connect_to("127.0.0.1", port);
    CHECK(c >= 0, "connect");
    struct sockaddr_in pa; socklen_t pl = sizeof pa;
    int a = accept(l, (void *)&pa, &pl);
    CHECK(a >= 0 && pl == sizeof pa && pa.sin_addr.s_addr == htonl(0x7f000001), "accept + peer address");
    struct sockaddr_in cn, ap; socklen_t x = sizeof cn, y = sizeof ap;
    getsockname(c, (void *)&cn, &x);
    getpeername(a, (void *)&ap, &y);
    CHECK(cn.sin_port == ap.sin_port && cn.sin_port == pa.sin_port, "names agree");
    CHECK(write(c, "hello", 5) == 5, "write");
    char b[16] = { 0 };
    CHECK(read(a, b, sizeof b) == 5 && !memcmp(b, "hello", 5), "read");
    CHECK(send(a, "world!", 6, 0) == 6, "send");
    CHECK(recv(c, b, sizeof b, 0) == 6 && !memcmp(b, "world!", 6), "recv");
    CHECK(shutdown(c, SHUT_WR) == 0, "shutdown");
    CHECK(read(a, b, sizeof b) == 0, "EOF after shutdown");
    CHECK(write(a, "late", 4) == 4, "half-closed peer can still send");
    CHECK(read(c, b, sizeof b) == 4, "read after own shutdown(WR)");
    close(a);
    CHECK(read(c, b, sizeof b) == 0, "EOF after peer close");
    close(c);
    /* 127.0.0.2 is local too, and getsockname reports the address used */
    CHECK(connect_to("127.0.0.2", port) < 0 && errno == ECONNREFUSED, "listener bound to 127.0.0.1 refuses 127.0.0.2");
    close(l);
    int port2; l = listener("0.0.0.0", 0, 4, &port2);
    int c2 = connect_to("127.0.0.2", port2);
    int a2 = c2 >= 0 ? accept(l, NULL, NULL) : -1;
    struct sockaddr_in me; socklen_t ml = sizeof me;
    getsockname(a2, (void *)&me, &ml);
    CHECK(c2 >= 0 && a2 >= 0 && me.sin_addr.s_addr == htonl(0x7f000002), "connect to 127.0.0.2");
    close(c2); close(a2); close(l);
    OK("tcp basic");
}

static void tcp_bulk(void) {
    int port, l = listener("127.0.0.1", 0, 4, &port);
    const size_t total = 16 << 20;
    pid_t pid = fork();
    if (pid == 0) {
        int c = connect_to("127.0.0.1", port);
        static uint8_t buf[100000];
        size_t off = 0;
        while (off < total) {
            size_t n = 1 + rnd() % sizeof buf;
            if (n > total - off) n = total - off;
            for (size_t i = 0; i < n; i++) buf[i] = pat(off + i, 7);
            if (writen(c, buf, n) != (ssize_t)n) _exit(2);
            off += n;
        }
        /* read back the echo of the server's checksum */
        uint32_t sum;
        if (readn(c, &sum, 4) != 4) _exit(3);
        close(c);
        _exit(0);
    }
    int a = accept(l, NULL, NULL);
    static uint8_t buf[70000];
    size_t got = 0, bad = 0;
    uint64_t t0 = now_ms();
    for (;;) {
        ssize_t r = read(a, buf, 1 + rnd() % sizeof buf);
        if (r <= 0) break;
        for (ssize_t i = 0; i < r; i++) if (buf[i] != pat(got + i, 7)) bad++;
        got += r;
        if (got == total) break;
    }
    uint64_t ms = now_ms() - t0;
    uint32_t sum = (uint32_t)got;
    write(a, &sum, 4);
    int st;
    waitpid(pid, &st, 0);
    CHECK(got == total && !bad, "bulk transfer: %zu of %zu bytes, %zu corrupt", got, total, bad);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "sender status %x", st);
    close(a); close(l);
    printf("nettest: tcp bulk ok (16 MiB in %llu ms, %llu MiB/s)\n", (unsigned long long)ms, (unsigned long long)(ms ? 16000 / ms : 0));
}

/* several clients at once against a poll()-driven echo server */
static void tcp_concurrent(void) {
    uint64_t t0 = now_ms();
    int port, l = listener("127.0.0.1", 0, 16, &port);
    enum { N = 8, PER = 300000 };
    pid_t kids[N];
    for (int k = 0; k < N; k++) {
        if ((kids[k] = fork()) == 0) {
            snprintf(stagebuf, sizeof stagebuf, "client %d", k); stage = stagebuf;
            int c = connect_to("127.0.0.1", port);
            if (c < 0) _exit(1);
            static uint8_t out[PER], in[PER];
            for (int i = 0; i < PER; i++) out[i] = pat(i, k);
            pid_t w = fork();
            if (w == 0) { snprintf(stagebuf, sizeof stagebuf, "writer %d", k); xfer = 0; if (writen(c, out, PER) != PER) _exit(2); shutdown(c, SHUT_WR); _exit(0); }
            ssize_t r = readn(c, in, PER);
            int st; waitpid(w, &st, 0);
            if (r != PER || memcmp(in, out, PER)) _exit(3);
            char z;
            if (read(c, &z, 1) != 0) _exit(4);
            _exit(WIFEXITED(st) && !WEXITSTATUS(st) ? 0 : 5);
        }
    }
    struct pollfd p[N + 1];
    int n = 1, done = 0;
    p[0] = (struct pollfd){ l, POLLIN, 0 };
    static uint8_t buf[N + 1][8192];
    while (done < N) {
        int r = poll(p, n, 20000);
        if (r <= 0) { CHECK(0, "poll timeout/err (done %d)", done); break; }
        if (p[0].revents & POLLIN) { int a = accept(l, NULL, NULL); if (a >= 0) p[n++] = (struct pollfd){ a, POLLIN, 0 }; }
        for (int i = 1; i < n; i++) {
            if (p[i].fd < 0 || !(p[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t k = read(p[i].fd, buf[i], sizeof buf[i]);
            if (k <= 0) { close(p[i].fd); p[i].fd = -1; done++; continue; }
            if (writen(p[i].fd, buf[i], k) != k) { close(p[i].fd); p[i].fd = -1; done++; }
        }
    }
    int good = 0;
    for (int k = 0; k < N; k++) { int st; waitpid(kids[k], &st, 0); if (WIFEXITED(st) && !WEXITSTATUS(st)) good++; else printf("nettest: client %d status %x\n", k, st); }
    CHECK(good == N, "%d of %d concurrent echo clients ok", good, N);
    close(l);
    printf("nettest: tcp concurrent ok (%llu ms)\n", (unsigned long long)(now_ms() - t0));
}

static void tcp_errors(void) {
    /* refused */
    int port, l = listener("127.0.0.1", 0, 1, &port);
    close(l);
    errno = 0;
    CHECK(connect_to("127.0.0.1", port) < 0 && errno == ECONNREFUSED, "connect to a closed port");
    /* nonblocking connect: refused, reported through poll + SO_ERROR */
    int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_in a = sin_of("127.0.0.1", port);
    int r = connect(s, (void *)&a, sizeof a);
    CHECK(r < 0 && errno == EINPROGRESS, "nonblocking connect EINPROGRESS");
    struct pollfd p = { s, POLLOUT, 0 };
    CHECK(poll(&p, 1, 3000) == 1 && (p.revents & (POLLERR | POLLHUP)), "poll reports the failure (%x)", p.revents);
    int err = 0; socklen_t el = sizeof err;
    CHECK(getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == ECONNREFUSED, "SO_ERROR=%d", err);
    close(s);
    /* nonblocking connect that succeeds; accept EAGAIN on an empty nonblocking listener */
    l = listener("127.0.0.1", 0, 4, &port);
    int fl = fcntl(l, F_GETFL);
    fcntl(l, F_SETFL, fl | O_NONBLOCK);
    CHECK(accept(l, NULL, NULL) < 0 && errno == EAGAIN, "accept EAGAIN");
    s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    a = sin_of("127.0.0.1", port);
    r = connect(s, (void *)&a, sizeof a);
    CHECK(r == 0 || errno == EINPROGRESS, "nonblocking connect");
    p = (struct pollfd){ s, POLLOUT, 0 };
    CHECK(poll(&p, 1, 3000) == 1 && (p.revents & POLLOUT) && !(p.revents & POLLERR), "connected: POLLOUT (%x)", p.revents);
    el = sizeof err;
    CHECK(getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0, "SO_ERROR 0");
    r = connect(s, (void *)&a, sizeof a);
    CHECK(r < 0 && errno == EISCONN, "second connect EISCONN");
    struct pollfd lp = { l, POLLIN, 0 };
    CHECK(poll(&lp, 1, 3000) == 1, "listener readable");
    int acc = accept(l, NULL, NULL);
    CHECK(acc >= 0, "accept");
    /* EADDRINUSE */
    int d = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in b = sin_of("0.0.0.0", port);
    CHECK(bind(d, (void *)&b, sizeof b) < 0 && errno == EADDRINUSE, "bind to a listening port");
    int one = 1;
    setsockopt(d, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    CHECK(bind(d, (void *)&b, sizeof b) < 0 && errno == EADDRINUSE, "SO_REUSEADDR does not steal a listener");
    close(d);
    /* bind to a foreign address */
    d = socket(AF_INET, SOCK_STREAM, 0);
    b = sin_of("192.0.2.1", 0);
    CHECK(bind(d, (void *)&b, sizeof b) < 0 && errno == EADDRNOTAVAIL, "bind to a foreign address");
    close(d);
    /* EPIPE after the peer is gone */
    signal(SIGPIPE, SIG_IGN);
    close(acc);
    usleep(100000);
    char buf[4096] = { 0 };
    ssize_t w = 0;
    for (int i = 0; i < 50 && (w >= 0 || errno == EAGAIN); i++) { w = send(s, buf, sizeof buf, MSG_NOSIGNAL); usleep(20000); }
    CHECK(w < 0 && (errno == EPIPE || errno == ECONNRESET), "write to a closed peer: EPIPE/ECONNRESET");
    close(s);
    /* closing with unread data resets the connection */
    fcntl(l, F_SETFL, fl);
    int c = connect_to("127.0.0.1", port);
    acc = accept(l, NULL, NULL);
    write(c, "unread", 6);
    usleep(50000);
    close(acc);
    usleep(50000);
    r = read(c, buf, sizeof buf);
    CHECK(r < 0 && errno == ECONNRESET, "RST on close with unread data (r=%d)", r);
    close(c);
    close(l);
    /* after closing everything, the port can be bound again with SO_REUSEADDR (TIME_WAIT) */
    l = listener("127.0.0.1", port, 1, NULL);
    CHECK(l >= 0, "rebind the port with SO_REUSEADDR");
    close(l);
    OK("tcp errors");
}

static void tcp_options(void) {
    int port, l = listener("127.0.0.1", 0, 4, &port);
    int c = connect_to("127.0.0.1", port), a = accept(l, NULL, NULL);
    int v = 1; socklen_t vl = sizeof v;
    CHECK(setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &v, sizeof v) == 0, "TCP_NODELAY");
    v = 0;
    CHECK(getsockopt(c, IPPROTO_TCP, TCP_NODELAY, &v, &vl) == 0 && v == 1, "TCP_NODELAY readback");
    vl = sizeof v;
    CHECK(getsockopt(c, SOL_SOCKET, SO_TYPE, &v, &vl) == 0 && v == SOCK_STREAM, "SO_TYPE");
    /* RFC 7323 window scaling + timestamps and RFC 2018 SACK were negotiated on both ends */
    for (int i = 0; i < 2; i++) {
        struct tcp_info ti; socklen_t tl = sizeof ti;
        memset(&ti, 0, sizeof ti);
        CHECK(getsockopt(i ? a : c, IPPROTO_TCP, TCP_INFO, &ti, &tl) == 0, "TCP_INFO");
        CHECK((ti.tcpi_options & 7) == 7, "tcpi_options=%d (want TS|SACK|WSCALE)", ti.tcpi_options);
        CHECK(ti.tcpi_rcv_wscale >= 1 && ti.tcpi_snd_wscale >= 1, "wscale snd=%d rcv=%d", ti.tcpi_snd_wscale, ti.tcpi_rcv_wscale);
    }
    vl = sizeof v;
    CHECK(getsockopt(c, IPPROTO_TCP, TCP_MAXSEG, &v, &vl) == 0 && v > 1000, "TCP_MAXSEG=%d", v);
    vl = sizeof v;
    CHECK(getsockopt(l, SOL_SOCKET, SO_ACCEPTCONN, &v, &vl) == 0 && v == 1, "SO_ACCEPTCONN");
    v = 1;
    CHECK(setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &v, sizeof v) == 0, "SO_KEEPALIVE");
    /* MSG_PEEK, FIONREAD, MSG_WAITALL */
    write(c, "abcdef", 6);
    usleep(50000);
    int n = 0;
    CHECK(ioctl(a, FIONREAD, &n) == 0 && n == 6, "FIONREAD=%d", n);
    char b[16] = { 0 };
    CHECK(recv(a, b, 3, MSG_PEEK) == 3 && !memcmp(b, "abc", 3), "MSG_PEEK");
    CHECK(recv(a, b, 6, 0) == 6 && !memcmp(b, "abcdef", 6), "data after peek");
    pid_t pid = fork();
    if (pid == 0) { for (int i = 0; i < 4; i++) { usleep(30000); write(c, "xy", 2); } _exit(0); }
    CHECK(recv(a, b, 8, MSG_WAITALL) == 8, "MSG_WAITALL collects 8 bytes");
    waitpid(pid, NULL, 0);
    /* SO_RCVTIMEO */
    struct timeval tv = { 0, 200000 };
    CHECK(setsockopt(a, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0, "SO_RCVTIMEO");
    uint64_t t0 = now_ms();
    CHECK(recv(a, b, 1, 0) < 0 && errno == EAGAIN, "recv times out");
    uint64_t dt = now_ms() - t0;
    CHECK(dt >= 150 && dt < 2000, "timeout took %llu ms", (unsigned long long)dt);
    /* epoll */
    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = a };
    epoll_ctl(ep, EPOLL_CTL_ADD, a, &ev);
    CHECK(epoll_wait(ep, &ev, 1, 0) == 0, "epoll: nothing yet");
    write(c, "!", 1);
    CHECK(epoll_wait(ep, &ev, 1, 2000) == 1 && ev.data.fd == a, "epoll: readable");
    read(a, b, 1);
    close(ep);
    /* /proc/net/tcp shows the listener */
    FILE *f = fopen("/proc/net/tcp", "r");
    char line[256]; int found = 0;
    char want[32]; snprintf(want, sizeof want, ":%04X 00000000:0000 0A", port);
    while (f && fgets(line, sizeof line, f)) if (strstr(line, want)) found = 1;
    if (f) fclose(f);
    CHECK(found, "/proc/net/tcp lists the listener");
    close(c); close(a); close(l);
    OK("tcp options");
}

/* a receiver that stops reading: the sender fills the window, blocks (EAGAIN), and resumes when
 * the receiver drains; exercises the zero window, persist timer and window updates */
/* RFC 7323: a receive buffer above 64 KiB is usable only through window scaling */
static void tcp_bigwindow(void) {
    int port, l = socket(AF_INET, SOCK_STREAM, 0);
    int big = 1 << 20;
    setsockopt(l, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    struct sockaddr_in a = sin_of("127.0.0.1", 0);
    bind(l, (void *)&a, sizeof a); listen(l, 1);
    socklen_t al = sizeof a; getsockname(l, (void *)&a, &al); port = ntohs(a.sin_port);
    int c = connect_to("127.0.0.1", port), s = accept(l, NULL, NULL);
    int v = 0; socklen_t vl = sizeof v;
    CHECK(getsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, &vl) == 0 && v == 2 << 20, "accepted SO_RCVBUF=%d", v);
    fcntl(c, F_SETFL, O_NONBLOCK);
    static uint8_t buf[4 << 20];
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = pat(i, 5);
    size_t sent = 0;
    for (int idle = 0; idle < 5 && sent < sizeof buf; ) {
        ssize_t w = write(c, buf + sent, sizeof buf - sent);
        if (w > 0) { sent += w; idle = 0; } else { idle++; usleep(50000); }
    }
    CHECK(sent > (1 << 20) + (256 << 10), "unread receiver absorbed %zu bytes (window > 64 KiB)", sent);
    static uint8_t in[4 << 20];
    size_t got = 0, bad = 0;
    struct pollfd p[2] = { { s, POLLIN, 0 }, { c, POLLOUT, 0 } };
    uint64_t t0 = now_ms();
    while (got < sizeof buf && now_ms() - t0 < 20000) {
        p[1].events = sent < sizeof buf ? POLLOUT : 0;
        poll(p, 2, 1000);
        if (p[1].revents & POLLOUT) { ssize_t w = write(c, buf + sent, sizeof buf - sent); if (w > 0) sent += w; }
        if (p[0].revents & POLLIN) { ssize_t r = read(s, in + got, sizeof in - got); if (r > 0) got += r; else break; }
    }
    for (size_t i = 0; i < got; i++) if (in[i] != buf[i]) bad++;
    CHECK(got == sizeof buf && !bad, "scaled-window transfer: %zu bytes, %zu corrupt", got, bad);
    close(c); close(s); close(l);
    OK("tcp window scaling");
}

static void tcp_flow(void) {
    int port, l = socket(AF_INET, SOCK_STREAM, 0);
    int small = 4096;
    setsockopt(l, SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    struct sockaddr_in a = sin_of("127.0.0.1", 0);
    bind(l, (void *)&a, sizeof a); listen(l, 1);
    socklen_t al = sizeof a; getsockname(l, (void *)&a, &al); port = ntohs(a.sin_port);
    int c = connect_to("127.0.0.1", port), s = accept(l, NULL, NULL);
    fcntl(c, F_SETFL, O_NONBLOCK);
    static uint8_t buf[1 << 20];
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = pat(i, 3);
    size_t sent = 0;
    for (;;) { ssize_t w = write(c, buf + sent, sizeof buf - sent); if (w <= 0) break; sent += w; }
    CHECK(errno == EAGAIN && sent < sizeof buf, "sender blocks (sent %zu)", sent);
    usleep(300000);
    size_t got = 0, bad = 0;
    static uint8_t in[1 << 20];
    struct pollfd p[2] = { { s, POLLIN, 0 }, { c, POLLOUT, 0 } };
    uint64_t t0 = now_ms();
    while (got < sizeof buf && now_ms() - t0 < 20000) {
        p[1].events = sent < sizeof buf ? POLLOUT : 0;
        poll(p, 2, 1000);
        if (p[1].revents & POLLOUT) { ssize_t w = write(c, buf + sent, sizeof buf - sent); if (w > 0) sent += w; }
        if (p[0].revents & POLLIN) { ssize_t r = read(s, in + got, 1000); if (r > 0) got += r; else break; }
    }
    for (size_t i = 0; i < got; i++) if (in[i] != buf[i]) bad++;
    CHECK(got == sizeof buf && !bad, "flow-controlled transfer: %zu bytes, %zu corrupt", got, bad);
    close(c); close(s); close(l);
    OK("tcp flow control");
}

/* ---------------------------------------------------------------- UDP */
static void udp_tests(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0), t = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = sin_of("127.0.0.1", 0), from;
    CHECK(bind(s, (void *)&a, sizeof a) == 0, "bind");
    socklen_t al = sizeof a;
    getsockname(s, (void *)&a, &al);
    CHECK(sendto(t, "ping", 4, 0, (void *)&a, sizeof a) == 4, "sendto");
    char b[70000];
    socklen_t fl = sizeof from;
    CHECK(recvfrom(s, b, sizeof b, 0, (void *)&from, &fl) == 4 && !memcmp(b, "ping", 4), "recvfrom");
    struct sockaddr_in tn; socklen_t tl = sizeof tn;
    getsockname(t, (void *)&tn, &tl);
    CHECK(from.sin_port == tn.sin_port && tn.sin_port != 0, "source port is the autobound port");
    CHECK(sendto(s, "pong", 4, 0, (void *)&from, fl) == 4, "reply");
    CHECK(recv(t, b, sizeof b, 0) == 4, "reply received");
    /* message boundaries and MSG_TRUNC */
    sendto(t, "0123456789", 10, 0, (void *)&a, sizeof a);
    sendto(t, "ab", 2, 0, (void *)&a, sizeof a);
    struct iovec iov = { b, 4 };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
    CHECK(recvmsg(s, &m, 0) == 4 && (m.msg_flags & MSG_TRUNC), "truncated datagram");
    CHECK(recv(s, b, sizeof b, MSG_TRUNC) == 2, "next datagram intact");
    /* a 60000-byte datagram */
    static char big[60000];
    for (int i = 0; i < (int)sizeof big; i++) big[i] = (char)pat(i, 9);
    CHECK(sendto(t, big, sizeof big, 0, (void *)&a, sizeof a) == sizeof big, "large sendto");
    CHECK(recv(s, b, sizeof b, 0) == sizeof big && !memcmp(b, big, sizeof big), "large datagram");
    /* connected socket: errors from ICMP port unreachable */
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in dead = sin_of("127.0.0.1", 9);   /* discard: nobody listens */
    CHECK(connect(u, (void *)&dead, sizeof dead) == 0, "connect");
    send(u, "x", 1, 0);
    usleep(100000);
    CHECK(recv(u, b, 1, MSG_DONTWAIT) < 0 && errno == ECONNREFUSED, "ECONNREFUSED from port unreachable");
    close(u);
    /* broadcast needs SO_BROADCAST */
    struct sockaddr_in bc = sin_of("255.255.255.255", 9);
    int r = sendto(t, "b", 1, 0, (void *)&bc, sizeof bc);
    CHECK(r < 0 && (errno == EACCES || errno == ENETUNREACH), "broadcast without SO_BROADCAST");
    /* nonblocking empty recv; poll */
    CHECK(recv(s, b, 1, MSG_DONTWAIT) < 0 && errno == EAGAIN, "EAGAIN");
    struct pollfd p = { s, POLLIN, 0 };
    sendto(t, "p", 1, 0, (void *)&a, sizeof a);
    CHECK(poll(&p, 1, 2000) == 1 && (p.revents & POLLIN), "poll POLLIN");
    int n = 0;
    CHECK(ioctl(s, FIONREAD, &n) == 0 && n == 1, "FIONREAD");
    recv(s, b, 1, 0);
    /* two sockets on one port need SO_REUSEADDR on both */
    int x = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(bind(x, (void *)&a, sizeof a) < 0 && errno == EADDRINUSE, "EADDRINUSE");
    close(x);
    close(s); close(t);
    OK("udp");
}

/* lower lo's MTU so datagrams get fragmented and reassembled */
static void ip_fragments(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq q = { 0 };
    strcpy(q.ifr_name, "lo");
    CHECK(ioctl(s, SIOCGIFMTU, &q) == 0, "SIOCGIFMTU");
    int old = q.ifr_mtu;
    q.ifr_mtu = 1500;
    CHECK(ioctl(s, SIOCSIFMTU, &q) == 0, "SIOCSIFMTU");
    struct sockaddr_in a = sin_of("127.0.0.1", 0);
    bind(s, (void *)&a, sizeof a);
    socklen_t al = sizeof a; getsockname(s, (void *)&a, &al);
    static char big[30000], in[40000];
    for (int i = 0; i < (int)sizeof big; i++) big[i] = (char)pat(i, 5);
    CHECK(sendto(s, big, sizeof big, 0, (void *)&a, sizeof a) == sizeof big, "fragmented sendto");
    CHECK(recv(s, in, sizeof in, 0) == sizeof big && !memcmp(in, big, sizeof big), "reassembled");
    /* TCP adapts its segment size to the MTU */
    int port, l = listener("127.0.0.1", 0, 1, &port);
    int c = connect_to("127.0.0.1", port), d = accept(l, NULL, NULL);
    int mss = 0; socklen_t ml = sizeof mss;
    getsockopt(c, IPPROTO_TCP, TCP_MAXSEG, &mss, &ml);
    CHECK(mss == 1460, "MSS %d at MTU 1500", mss);
    CHECK(writen(c, big, sizeof big) == sizeof big && readn(d, in, sizeof big) == sizeof big && !memcmp(in, big, sizeof big), "TCP at MTU 1500");
    close(c); close(d); close(l);
    q.ifr_mtu = old;
    ioctl(s, SIOCSIFMTU, &q);
    FILE *f = fopen("/proc/net/snmp", "r");
    char line[512]; int ok = 0;
    while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "Ip: 2", 5)) { unsigned long v[7]; if (sscanf(line + 4, "%*d %*d %lu %lu %lu %lu %lu %lu", v, v + 1, v + 2, v + 3, v + 4, v + 5) == 6 && v[4] > 0 && v[5] > 0) ok = 1; }
    if (f) fclose(f);
    CHECK(ok, "/proc/net/snmp counts fragments and reassemblies");
    close(s);
    OK("ip fragmentation");
}

/* ---------------------------------------------------------------- ICMP */
static uint16_t cksum(const void *d, int n) {
    const uint16_t *p = d; uint32_t s = 0;
    for (; n > 1; n -= 2) s += *p++;
    if (n) s += *(const uint8_t *)p;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}
static void icmp_tests(void) {
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    CHECK(s >= 0, "raw ICMP socket as root");
    struct icmphdr h = { .type = ICMP_ECHO, .un.echo = { htons(0x1234), htons(1) } };
    char pkt[64] = { 0 };
    memcpy(pkt, &h, sizeof h);
    strcpy(pkt + sizeof h, "9os ping");
    ((struct icmphdr *)pkt)->checksum = cksum(pkt, sizeof pkt);
    struct sockaddr_in a = sin_of("127.0.0.1", 0);
    CHECK(sendto(s, pkt, sizeof pkt, 0, (void *)&a, sizeof a) == sizeof pkt, "send echo");
    char in[256];
    int got = 0;
    struct timeval tv = { 2, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    for (int i = 0; i < 4 && !got; i++) {
        ssize_t r = recv(s, in, sizeof in, 0);
        if (r < 28) break;
        struct ip *ip = (struct ip *)in;
        struct icmphdr *ic = (struct icmphdr *)(in + ip->ip_hl * 4);
        if (ic->type == ICMP_ECHOREPLY && ic->un.echo.id == htons(0x1234) && !strcmp((char *)(ic + 1), "9os ping")) got = 1;
    }
    CHECK(got, "echo reply on the raw socket");
    close(s);
    /* unprivileged-style ping socket: the kernel owns the identifier */
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    CHECK(s >= 0, "ICMP datagram socket");
    h.checksum = 0;
    CHECK(sendto(s, &h, sizeof h, 0, (void *)&a, sizeof a) == sizeof h, "send on ping socket");
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ssize_t r = recv(s, in, sizeof in, 0);
    CHECK(r == sizeof h && ((struct icmphdr *)in)->type == ICMP_ECHOREPLY, "ping socket reply (%zd)", r);
    close(s);
    OK("icmp");
}

/* ---------------------------------------------------------------- privileges, interfaces */
static void privileges(void) {
    pid_t pid = fork();
    if (pid == 0) {
        if (setgid(1000) || setuid(1000)) _exit(10);
        int bad = 0;
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = sin_of("0.0.0.0", 80);
        if (!(bind(s, (void *)&a, sizeof a) < 0 && errno == EACCES)) bad |= 1;
        a = sin_of("0.0.0.0", 0);
        if (bind(s, (void *)&a, sizeof a)) bad |= 2;
        if (!(socket(AF_INET, SOCK_RAW, IPPROTO_ICMP) < 0 && errno == EPERM)) bad |= 4;
        if (!(socket(AF_PACKET, SOCK_RAW, 0) < 0 && errno == EPERM)) bad |= 8;
        struct ifreq q = { 0 };
        strcpy(q.ifr_name, "lo");
        if (ioctl(s, SIOCGIFFLAGS, &q)) bad |= 16;
        if (!(ioctl(s, SIOCSIFFLAGS, &q) < 0 && errno == EPERM)) bad |= 32;
        _exit(bad);
    }
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "unprivileged restrictions (mask %d)", WEXITSTATUS(st));
    /* interface ioctls */
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq q = { 0 };
    strcpy(q.ifr_name, "lo");
    CHECK(ioctl(s, SIOCGIFADDR, &q) == 0 && ((struct sockaddr_in *)&q.ifr_addr)->sin_addr.s_addr == htonl(0x7f000001), "SIOCGIFADDR lo");
    CHECK(ioctl(s, SIOCGIFINDEX, &q) == 0 && q.ifr_ifindex == 1, "SIOCGIFINDEX lo");
    struct ifreq v[8]; struct ifconf ic = { .ifc_len = sizeof v, .ifc_req = v };
    CHECK(ioctl(s, SIOCGIFCONF, &ic) == 0 && ic.ifc_len >= (int)sizeof(struct ifreq) && !strcmp(v[0].ifr_name, "lo"), "SIOCGIFCONF");
    CHECK(socket(AF_INET6, SOCK_STREAM, 0) < 0 && errno == EAFNOSUPPORT, "no IPv6");
    close(s);
    OK("privileges and interfaces");
}

/* ---------------------------------------------------------------- external (host server) */
static int external(const char *ip, int port) {
    /* TCP echo of 4 MiB */
    int c = connect_to(ip, port);
    CHECK(c >= 0, "connect to %s:%d", ip, port);
    if (c < 0) return 1;
    const size_t total = 4 << 20;
    pid_t pid = fork();
    if (pid == 0) {
        static uint8_t b[65536];
        for (size_t off = 0; off < total; off += sizeof b) { for (size_t i = 0; i < sizeof b; i++) b[i] = pat(off + i, 11); if (writen(c, b, sizeof b) != sizeof b) _exit(1); }
        shutdown(c, SHUT_WR);
        _exit(0);
    }
    static uint8_t in[65536];
    size_t got = 0, bad = 0;
    uint64_t t0 = now_ms();
    for (;;) { ssize_t r = read(c, in, sizeof in); if (r <= 0) break; for (ssize_t i = 0; i < r; i++) if (in[i] != pat(got + i, 11)) bad++; got += r; }
    int st; waitpid(pid, &st, 0);
    uint64_t ms = now_ms() - t0;
    CHECK(got == total && !bad && WIFEXITED(st) && !WEXITSTATUS(st), "external echo: %zu/%zu bytes, %zu corrupt", got, total, bad);
    close(c);
    printf("nettest: external tcp echo ok (4 MiB round trip in %llu ms)\n", (unsigned long long)ms);
    /* UDP echo */
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = sin_of(ip, port);
    connect(u, (void *)&a, sizeof a);
    struct timeval tv = { 1, 0 };
    setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int ok = 0;
    char b[2000];
    for (int i = 0; i < 5 && !ok; i++) {
        memset(b, 'u' + i, 1400);
        send(u, b, 1400, 0);
        char r[2000];
        if (recv(u, r, sizeof r, 0) == 1400 && !memcmp(r, b, 1400)) ok = 1;
    }
    CHECK(ok, "external udp echo");
    close(u);
    OK("external udp echo");
    return fails != 0;
}

static int serve(int port) {
    int l = listener("0.0.0.0", port, 8, NULL);
    if (l < 0) { perror("nettest: listen"); return 1; }
    if (fork()) { printf("nettest: echo server on port %d\n", port); return 0; }
    setsid();
    for (int n = 0; n < 16; n++) {
        int a = accept(l, NULL, NULL);
        if (a < 0) continue;
        if (fork() == 0) {
            static char b[65536];
            ssize_t r;
            while ((r = read(a, b, sizeof b)) > 0) if (writen(a, b, r) != r) break;
            _exit(0);
        }
        close(a);
    }
    _exit(0);
}

static void on_alarm(int sig) {
    (void)sig;
    char b[128];
    int n = snprintf(b, sizeof b, "nettest: FAIL %s timed out (xfer %zu)\n", stage, (size_t)xfer);
    write(1, b, n);
    if (strncmp(stage, "client", 6) && strncmp(stage, "writer", 6)) {
        if (fork() == 0) {
            execl("/bin/sh", "sh", "-c", "cat /proc/net/tcp; for p in $(pidof nettest); do echo $p $(grep State /proc/$p/status) $(cat /proc/$p/stat | cut -d' ' -f3-8); done", (char *)NULL);
            _exit(1);
        }
        sleep(3);
        _exit(2);
    }
}
#define RUN(fn) do { stage = #fn; alarm(120); fn(); alarm(0); } while (0)

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    verbose = getenv("NETTEST_V") != NULL;
    if (argc == 4 && !strcmp(argv[1], "-x")) return external(argv[2], atoi(argv[3]));
    if (argc == 3 && !strcmp(argv[1], "-s")) return serve(atoi(argv[2]));
    signal(SIGPIPE, SIG_IGN);
    signal(SIGALRM, on_alarm);
    RUN(tcp_basic);
    RUN(tcp_errors);
    RUN(tcp_options);
    RUN(tcp_bulk);
    RUN(tcp_concurrent);
    RUN(tcp_flow);
    RUN(tcp_bigwindow);
    RUN(udp_tests);
    RUN(ip_fragments);
    RUN(icmp_tests);
    RUN(privileges);
    printf("nettest: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
