/* Exact source runs on Linux/9os. No timing/performance acceptance thresholds.
 * Connected data wakes must preserve aliases, poll/ppoll/direct/nested epoll, blocking
 * reads, full-buffer write readiness and global peer close/shutdown semantics. */
#define _GNU_SOURCE
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
#include <sys/socket.h>
#include <unistd.h>
static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("unixpolltest: FAIL line %d: %s\n", __LINE__, #x); } } while (0)
struct action { int fd, op, rc; size_t bytes; };
static const uint64_t value = 0x123456789abcdef0ULL;
static void *later(void *ptr) {
    struct action *a = ptr;
    usleep(30000);
    if (a->op == 0) a->rc = write(a->fd, &value, sizeof value) == sizeof value ? 0 : -1;
    else if (a->op == 1) a->rc = shutdown(a->fd, SHUT_WR);
    else if (a->op == 2) a->rc = close(a->fd);
    else {
        char b[4096]; ssize_t n;
        while ((n = read(a->fd, b, sizeof b)) > 0) a->bytes += (size_t)n;
        a->rc = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    }
    return NULL;
}
static void readable(int type, int mode) {
    int s[2]; int r = socketpair(AF_UNIX, type | SOCK_CLOEXEC, 0, s);
    CHECK(r == 0); if (r) return;
    int alias = dup(s[0]), e = eventfd(0, EFD_NONBLOCK), inner = -1, outer = -1;
    CHECK(alias >= 0 && e >= 0);
    if (alias < 0 || e < 0) goto cleanup;
    CHECK(close(s[0]) == 0); s[0] = -1; /* dup is now the only receiver fd */
    int target = alias;
    if (mode == 2 || mode == 4) {
        inner = epoll_create1(EPOLL_CLOEXEC); CHECK(inner >= 0); if (inner < 0) goto cleanup;
        struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 0xabc};
        CHECK(epoll_ctl(inner, EPOLL_CTL_ADD, alias, &ev) == 0);
        if (mode == 2) {
            outer = epoll_create1(EPOLL_CLOEXEC); CHECK(outer >= 0); if (outer < 0) goto cleanup;
            ev.data.u64 = 0xdef; CHECK(epoll_ctl(outer, EPOLL_CTL_ADD, inner, &ev) == 0);
            target = outer;
        } else {
            ev.data.u64 = 0x123; CHECK(epoll_ctl(inner, EPOLL_CTL_ADD, e, &ev) == 0);
        }
    }
    struct action a = {s[1], 0, -1, 0}; pthread_t t;
    int made = pthread_create(&t, NULL, later, &a) == 0;
    CHECK(made); if (!made) goto cleanup;
    if (mode == 4) {
        struct epoll_event ev[2] = {{0}};
        CHECK(epoll_pwait(inner, ev, 2, 2000, NULL) == 1 && ev[0].data.u64 == 0xabc && ev[0].events == EPOLLIN);
    } else if (mode != 3) {
        struct pollfd p[2] = {{target, POLLIN, 0}, {e, POLLIN, 0}};
        struct timespec limit = {2, 0};
        r = mode == 1 ? ppoll(p, 2, &limit, NULL) : poll(p, 2, 2000);
        CHECK(r == 1 && (p[0].revents & POLLIN) && !p[1].revents);
        if (mode == 2) {
            struct epoll_event ev;
            CHECK(epoll_wait(outer, &ev, 1, 0) == 1 && ev.data.u64 == 0xdef && (ev.events & EPOLLIN));
            CHECK(epoll_wait(inner, &ev, 1, 0) == 1 && ev.data.u64 == 0xabc && (ev.events & EPOLLIN));
        }
    }
    uint64_t got = 0;
    CHECK(read(alias, &got, sizeof got) == sizeof got && got == value);
    CHECK(pthread_join(t, NULL) == 0 && a.rc == 0);
    errno = 0; CHECK(read(e, &got, sizeof got) == -1 && errno == EAGAIN);
cleanup:
    if (outer >= 0) close(outer);
    if (inner >= 0) close(inner);
    if (alias >= 0) close(alias);
    if (e >= 0) close(e);
    if (s[0] >= 0) close(s[0]);
    close(s[1]);
}
static void writable(int type, int direct_epoll) {
    int s[2]; int r = socketpair(AF_UNIX, type | SOCK_NONBLOCK, 0, s);
    CHECK(r == 0); if (r) return;
    int alias = dup(s[0]), ep = -1; CHECK(alias >= 0); if (alias < 0) goto done;
    CHECK(close(s[0]) == 0); s[0] = -1;
    char b[4096] = {0}; size_t total = 0; int full = 0;
    for (unsigned i = 0; i < 1024; i++) {
        ssize_t n = write(alias, b, sizeof b);
        if (n < 0) { full = errno == EAGAIN || errno == EWOULDBLOCK; break; }
        CHECK(n > 0); if (n <= 0) break; total += (size_t)n;
    }
    CHECK(full && total > 0);
    struct pollfd p = {alias, POLLOUT, 0}; CHECK(poll(&p, 1, 0) == 0);
    if (direct_epoll) {
        ep = epoll_create1(EPOLL_CLOEXEC); CHECK(ep >= 0); if (ep < 0) goto done;
        struct epoll_event ev = {.events = EPOLLOUT, .data.u64 = 0x456};
        CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, alias, &ev) == 0);
        CHECK(epoll_wait(ep, &ev, 1, 0) == 0);
    }
    struct action a = {s[1], 3, -1, 0}; pthread_t t;
    int made = pthread_create(&t, NULL, later, &a) == 0; CHECK(made);
    if (made) {
        if (direct_epoll) {
            struct epoll_event ev = {0};
            CHECK(epoll_pwait(ep, &ev, 1, 2000, NULL) == 1 && ev.events == EPOLLOUT && ev.data.u64 == 0x456);
        } else CHECK(poll(&p, 1, 2000) == 1 && (p.revents & POLLOUT));
        CHECK(pthread_join(t, NULL) == 0 && a.rc == 0 && a.bytes == total);
    }
done:
    if (ep >= 0) close(ep);
    if (alias >= 0) close(alias);
    if (s[0] >= 0) close(s[0]);
    close(s[1]);
}
static void peer_end(int type, int op, int direct_epoll) {
    int s[2]; int r = socketpair(AF_UNIX, type, 0, s);
    CHECK(r == 0); if (r) return;
    int ep = -1;
    if (direct_epoll) {
        ep = epoll_create1(EPOLL_CLOEXEC); CHECK(ep >= 0);
        struct epoll_event ev = {.events = EPOLLIN | EPOLLRDHUP, .data.u64 = 0x789};
        CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, s[0], &ev) == 0);
    }
    struct action a = {s[1], op, -1, 0}; pthread_t t;
    int made = pthread_create(&t, NULL, later, &a) == 0; CHECK(made);
    if (made) {
        unsigned events;
        if (direct_epoll) {
            struct epoll_event ev = {0};
            CHECK(epoll_pwait(ep, &ev, 1, 2000, NULL) == 1 && ev.data.u64 == 0x789);
            events = ev.events;
        } else {
            struct pollfd p = {s[0], POLLIN | POLLRDHUP, 0};
            CHECK(poll(&p, 1, 2000) == 1);
            events = p.revents;
        }
        CHECK((events & POLLIN) && (events & POLLRDHUP));
        if (op == 2) CHECK(events & POLLHUP);
        char b; CHECK(read(s[0], &b, 1) == 0);
        CHECK(pthread_join(t, NULL) == 0 && a.rc == 0);
    }
    if (ep >= 0) close(ep);
    close(s[0]); if (!made || op != 2) close(s[1]);
}
int main(int argc, char **argv) {
    (void)argv; if (argc != 1) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0); alarm(40);
    int types[] = {SOCK_STREAM, SOCK_SEQPACKET, SOCK_DGRAM};
    for (unsigned i = 0; i < 3; i++) for (int mode = 0; mode < 5; mode++) readable(types[i], mode);
    for (unsigned i = 0; i < 2; i++) for (int direct = 0; direct < 2; direct++) {
        writable(types[i], direct); peer_end(types[i], 1, direct); peer_end(types[i], 2, direct);
    }
    alarm(0);
    printf("unixpolltest: %s (%d checks, %d failures)\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures != 0;
}
