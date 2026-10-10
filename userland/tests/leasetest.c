/* File leases (M33): F_SETLEASE/F_GETLEASE rules (owner only, no conflicting opens, EAGAIN/
 * EINVAL/EACCES), lease breaks by read-only opens (write lease -> read), write opens and
 * truncate(2) (any lease -> none) with SIGIO or the F_SETSIG signal to the holder, the opener
 * blocking until the holder downgrades/releases (O_NONBLOCK: EWOULDBLOCK), the forced break
 * after /proc/sys/fs/lease-break-time, release on close, and /proc/locks. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("leasetest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" (errno %d)\n", errno); fails++; } } while (0)
static const char *F = "/tmp/lease_f";

static long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }

/* child: open F with flags (or truncate if flags < 0); exit 0 or errno; report elapsed ms in *pp */
static pid_t opener(int flags, int *rfd) {
    int p[2]; pipe(p);
    pid_t pid = fork();
    if (!pid) {
        close(p[0]);
        long t = ms();
        int r = flags < 0 ? truncate(F, 0) : open(F, flags);
        int e = r < 0 ? errno : 0;
        long el = ms() - t;
        write(p[1], &el, sizeof el);
        _exit(e);
    }
    close(p[1]);
    *rfd = p[0];
    return pid;
}
static int reap(pid_t pid, int rfd, long *el) {
    int st; waitpid(pid, &st, 0);
    *el = -1; read(rfd, el, sizeof *el); close(rfd);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}
static int waitsig(int sig, int msec) {
    sigset_t s; sigemptyset(&s); sigaddset(&s, sig);
    struct timespec ts = { msec / 1000, (msec % 1000) * 1000000L };
    return sigtimedwait(&s, NULL, &ts) == sig;
}
static void set_break_time(int s) {
    int fd = open("/proc/sys/fs/lease-break-time", O_WRONLY);
    char b[16]; int n = snprintf(b, sizeof b, "%d\n", s);
    CHECK(fd >= 0 && write(fd, b, n) == n, "set lease-break-time");
    close(fd);
}

int main(void) {
    sigset_t blk; sigemptyset(&blk); sigaddset(&blk, SIGIO); sigaddset(&blk, SIGUSR1);
    sigprocmask(SIG_BLOCK, &blk, NULL);
    unlink(F);
    int fd = open(F, O_CREAT | O_RDWR, 0644); write(fd, "data", 4); close(fd);
    set_break_time(10);

    /* rules */
    fd = open(F, O_RDONLY);
    CHECK(fcntl(fd, F_GETLEASE) == F_UNLCK, "no lease");
    CHECK(fcntl(fd, F_SETLEASE, F_UNLCK) < 0 && errno == EAGAIN, "unlock without lease");
    CHECK(fcntl(fd, F_SETLEASE, 7) < 0 && errno == EINVAL, "bad type");
    int d = open("/tmp", O_RDONLY);
    CHECK(fcntl(d, F_SETLEASE, F_RDLCK) < 0 && errno == EINVAL, "directory");
    close(d);
    int w = open(F, O_WRONLY);
    CHECK(fcntl(fd, F_SETLEASE, F_RDLCK) < 0 && errno == EAGAIN, "read lease with a writer");
    close(w);
    CHECK(fcntl(fd, F_SETLEASE, F_RDLCK) == 0 && fcntl(fd, F_GETLEASE) == F_RDLCK, "read lease");
    int r2 = open(F, O_RDONLY);
    CHECK(r2 >= 0, "read-only open with a read lease");
    CHECK(fcntl(r2, F_SETLEASE, F_RDLCK) == 0, "second read lease");
    CHECK(fcntl(fd, F_SETLEASE, F_WRLCK) < 0 && errno == EAGAIN, "write lease with another open");
    close(r2);
    CHECK(fcntl(fd, F_SETLEASE, F_WRLCK) == 0 && fcntl(fd, F_GETLEASE) == F_WRLCK, "upgrade to write lease");
    char lk[2048]; int pf = open("/proc/locks", O_RDONLY); int n = read(pf, lk, sizeof lk - 1); close(pf);
    lk[n > 0 ? n : 0] = 0;
    CHECK(strstr(lk, "LEASE  ACTIVE    WRITE") != NULL, "/proc/locks: %s", lk);
    pid_t pid = fork();
    if (!pid) {
        int f2 = open(F, O_RDONLY | O_NONBLOCK);   /* starts the break, fails with EWOULDBLOCK */
        if (f2 >= 0 || errno != EWOULDBLOCK) _exit(1);
        _exit(0);
    }
    int st; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "O_NONBLOCK open: EWOULDBLOCK");
    CHECK(waitsig(SIGIO, 1000), "SIGIO on break");
    CHECK(fcntl(fd, F_GETLEASE) == F_RDLCK, "breaking to read: %d", fcntl(fd, F_GETLEASE));
    CHECK(fcntl(fd, F_SETLEASE, F_WRLCK) < 0 && errno == EAGAIN, "no upgrade while breaking");
    CHECK(fcntl(fd, F_SETLEASE, F_RDLCK) == 0, "downgrade");
    CHECK(fcntl(fd, F_GETLEASE) == F_RDLCK, "read lease kept");

    /* read-only open waits for the write-lease holder to downgrade */
    CHECK(fcntl(fd, F_SETLEASE, F_WRLCK) == 0, "write lease again");
    int rp; pid = opener(O_RDONLY, &rp);
    CHECK(waitsig(SIGIO, 2000), "SIGIO");
    usleep(300000);
    CHECK(fcntl(fd, F_SETLEASE, F_RDLCK) == 0, "downgrade");
    long el; int e = reap(pid, rp, &el);
    CHECK(e == 0 && el >= 250 && el < 5000, "blocked open: err %d, %ld ms", e, el);

    /* write open breaks a read lease completely; F_SETSIG chooses the signal */
    CHECK(fcntl(fd, F_SETSIG, SIGUSR1) == 0 && fcntl(fd, F_GETSIG) == SIGUSR1, "F_SETSIG");
    pid = opener(O_WRONLY, &rp);
    CHECK(waitsig(SIGUSR1, 2000), "SIGUSR1 on break");
    CHECK(fcntl(fd, F_GETLEASE) == F_UNLCK, "breaking to none");
    CHECK(fcntl(fd, F_SETLEASE, F_RDLCK) < 0 && errno == EAGAIN, "read lease doesn't end a break to none");
    usleep(200000);
    CHECK(fcntl(fd, F_SETLEASE, F_UNLCK) == 0, "release");
    e = reap(pid, rp, &el);
    CHECK(e == 0 && el >= 150 && el < 5000, "write open: err %d, %ld ms", e, el);
    CHECK(fcntl(fd, F_SETSIG, 0) == 0, "F_SETSIG 0");

    /* truncate(2) breaks too */
    CHECK(fcntl(fd, F_SETLEASE, F_RDLCK) == 0, "read lease for truncate");
    pid = opener(-1, &rp);
    CHECK(waitsig(SIGIO, 2000), "SIGIO on truncate");
    usleep(200000);
    struct stat sb; stat(F, &sb);
    CHECK(sb.st_size == 4, "truncate waits (size %ld)", (long)sb.st_size);
    close(fd);                       /* closing releases the lease */
    e = reap(pid, rp, &el);
    stat(F, &sb);
    CHECK(e == 0 && sb.st_size == 0, "truncate after close: err %d size %ld", e, (long)sb.st_size);

    /* the holder ignores the break: forced after lease-break-time */
    set_break_time(1);
    fd = open(F, O_RDONLY);
    CHECK(fcntl(fd, F_SETLEASE, F_WRLCK) == 0, "write lease (forced)");
    pid = opener(O_RDONLY, &rp);
    e = reap(pid, rp, &el);
    CHECK(e == 0 && el >= 800 && el < 4000, "forced break: err %d, %ld ms", e, el);
    CHECK(fcntl(fd, F_GETLEASE) == F_RDLCK, "forced downgrade");
    pid = opener(O_RDWR, &rp);
    e = reap(pid, rp, &el);
    CHECK(e == 0 && el >= 800 && el < 4000 && fcntl(fd, F_GETLEASE) == F_UNLCK, "forced release: %d %ld", e, el);
    sigset_t s; sigemptyset(&s); sigaddset(&s, SIGIO);
    struct timespec z = { 0, 0 };
    while (sigtimedwait(&s, NULL, &z) == SIGIO) {}
    close(fd);
    set_break_time(45);

    /* only the owner (or CAP_LEASE) */
    pid = fork();
    if (!pid) {
        if (setgid(1000) || setuid(1000)) _exit(99);
        int f2 = open(F, O_RDONLY);
        if (f2 < 0) _exit(98);
        _exit(fcntl(f2, F_SETLEASE, F_RDLCK) < 0 && errno == EACCES ? 0 : 1);
    }
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "non-owner: EACCES (%x)", st);
    unlink(F);
    if (fails) { printf("leasetest: %d failures\n", fails); return 1; }
    printf("leasetest: all passed\n");
    return 0;
}
