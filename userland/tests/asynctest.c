/* Signal-driven I/O (O_ASYNC): F_SETOWN/F_GETOWN (process and process group), F_SETOWN_EX/
 * F_GETOWN_EX, F_SETSIG with siginfo (si_code POLL_IN, si_fd, si_band), FIOASYNC/FIOSETOWN on
 * a socketpair, re-arming after the reader drained the data, no signals once O_ASYNC is off,
 * and lease-break signals carrying si_fd + POLL_MSG. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("asynctest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" (errno %d)\n", errno); fflush(stdout); fails++; } } while (0)

static int waitsig(int sig, int msec, siginfo_t *si) {
    sigset_t s; sigemptyset(&s); sigaddset(&s, sig);
    struct timespec ts = { msec / 1000, (msec % 1000) * 1000000L };
    siginfo_t tmp;
    return sigtimedwait(&s, si ? si : &tmp, &ts) == sig;
}
static void drain(int fd) { char b[256]; while (read(fd, b, sizeof b) > 0) {} }

int main(void) {
    int RT = SIGRTMIN + 1;
    sigset_t blk; sigemptyset(&blk); sigaddset(&blk, SIGIO); sigaddset(&blk, RT);
    sigprocmask(SIG_BLOCK, &blk, NULL);
    int p[2];
    CHECK(pipe(p) == 0, "pipe");
    CHECK(fcntl(p[0], F_SETOWN, getpid()) == 0 && fcntl(p[0], F_GETOWN) == getpid(), "F_SETOWN/F_GETOWN");
    CHECK(fcntl(p[0], F_SETFL, O_ASYNC | O_NONBLOCK) == 0, "O_ASYNC");
    CHECK(fcntl(p[0], F_GETFL) & O_ASYNC, "F_GETFL shows O_ASYNC");
    CHECK(!waitsig(SIGIO, 200, NULL), "no SIGIO without data");
    write(p[1], "a", 1);
    CHECK(waitsig(SIGIO, 2000, NULL), "SIGIO on data");
    drain(p[0]);
    pid_t c = fork();
    if (!c) { usleep(100000); write(p[1], "b", 1); _exit(0); }
    CHECK(waitsig(SIGIO, 2000, NULL), "SIGIO again after draining (from another process)");
    waitpid(c, NULL, 0);
    drain(p[0]);

    CHECK(fcntl(p[0], F_SETSIG, RT) == 0 && fcntl(p[0], F_GETSIG) == RT, "F_SETSIG");
    write(p[1], "c", 1);
    siginfo_t si;
    memset(&si, 0, sizeof si);
    CHECK(waitsig(RT, 2000, &si), "F_SETSIG signal");
    CHECK(si.si_code == POLL_IN && si.si_fd == p[0] && (si.si_band & POLLIN), "siginfo code %d fd %d band %lx", si.si_code, si.si_fd, (long)si.si_band);
    drain(p[0]);
    CHECK(fcntl(p[0], F_SETSIG, 0) == 0, "F_SETSIG 0");

    struct f_owner_ex ox = { F_OWNER_PID, getpid() }, oy = { -1, -1 };
    CHECK(fcntl(p[0], F_SETOWN_EX, &ox) == 0 && fcntl(p[0], F_GETOWN_EX, &oy) == 0 && oy.type == F_OWNER_PID && oy.pid == getpid(), "OWN_EX");
    ox.type = 7;
    CHECK(fcntl(p[0], F_SETOWN_EX, &ox) < 0 && errno == EINVAL, "OWN_EX bad type");
    setpgid(0, 0);
    CHECK(fcntl(p[0], F_SETOWN, -getpgrp()) == 0 && fcntl(p[0], F_GETOWN) == -getpgrp(), "process group owner");
    write(p[1], "d", 1);
    CHECK(waitsig(SIGIO, 2000, NULL), "SIGIO to the process group");
    drain(p[0]);
    CHECK(fcntl(p[0], F_SETFL, O_NONBLOCK) == 0 && !(fcntl(p[0], F_GETFL) & O_ASYNC), "O_ASYNC off");
    write(p[1], "e", 1);
    CHECK(!waitsig(SIGIO, 300, NULL), "no SIGIO when off");
    close(p[0]); close(p[1]);

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    int pid = getpid(), on = 1, got = 0;
    CHECK(ioctl(sv[0], FIOSETOWN, &pid) == 0 && ioctl(sv[0], FIOGETOWN, &got) == 0 && got == pid, "FIOSETOWN/FIOGETOWN");
    CHECK(ioctl(sv[0], FIOASYNC, &on) == 0, "FIOASYNC");
    send(sv[1], "x", 1, 0);
    CHECK(waitsig(SIGIO, 2000, NULL), "SIGIO on socket data");
    char b[8];
    CHECK(recv(sv[0], b, sizeof b, MSG_DONTWAIT) == 1, "recv");
    send(sv[1], "y", 1, 0);
    CHECK(waitsig(SIGIO, 2000, NULL), "SIGIO after recv re-armed");
    close(sv[0]); close(sv[1]);

    /* lease break with F_SETSIG: si_fd and POLL_MSG */
    const char *F = "/tmp/async_lease";
    int fd = open(F, O_CREAT | O_RDWR, 0644); close(fd);
    fd = open(F, O_RDONLY);
    CHECK(fcntl(fd, F_SETSIG, RT) == 0 && fcntl(fd, F_SETLEASE, F_RDLCK) == 0, "lease");
    c = fork();
    if (!c) { int w = open(F, O_WRONLY | O_NONBLOCK); _exit(w < 0 && errno == EWOULDBLOCK ? 0 : 1); }
    int st; waitpid(c, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "nonblocking breaker");
    memset(&si, 0, sizeof si);
    CHECK(waitsig(RT, 2000, &si) && si.si_fd == fd && si.si_code == POLL_MSG, "lease siginfo fd %d code %d", si.si_fd, si.si_code);
    CHECK(fcntl(fd, F_SETLEASE, F_UNLCK) == 0, "release");
    close(fd); unlink(F);

    if (fails) { printf("asynctest: %d failures\n", fails); return 1; }
    printf("asynctest: all passed\n");
    return 0;
}
