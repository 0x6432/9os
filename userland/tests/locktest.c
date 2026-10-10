/* File locks (M33): flock(2) shared/exclusive/convert/LOCK_NB and release on last close (not
 * on dup close), POSIX fcntl record locks (split/merge, F_GETLK, F_SETLKW blocking across
 * fork, EDEADLK detection, any close releases, not inherited by fork), OFD locks (per open
 * file, F_OFD_GETLK reports pid -1, conflict with POSIX locks of other processes). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("locktest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" (errno %d)\n", errno); fails++; } } while (0)
#ifndef F_OFD_GETLK
#define F_OFD_GETLK 36
#define F_OFD_SETLK 37
#define F_OFD_SETLKW 38
#endif

static const char *path;
static void on_alrm(int s) { (void)s; }

static int lk(int fd, int cmd, int type, off_t start, off_t len) {
    struct flock fl = { .l_type = type, .l_whence = SEEK_SET, .l_start = start, .l_len = len };
    return fcntl(fd, cmd, &fl);
}
static struct flock getlk(int fd, int cmd, int type, off_t start, off_t len) {
    struct flock fl = { .l_type = type, .l_whence = SEEK_SET, .l_start = start, .l_len = len };
    if (fcntl(fd, cmd, &fl) < 0) fl.l_type = -1;
    return fl;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* run fn in a child; returns its exit status */
static int in_child(int (*fn)(void *), void *arg) {
    pid_t p = fork();
    if (!p) _exit(fn(arg));
    int st;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + WTERMSIG(st);
}

static int child_flock_nb(void *a) {
    int fd = open(path, O_RDWR);
    int r = flock(fd, *(int *)a | LOCK_NB);
    return r == 0 ? 0 : errno == EWOULDBLOCK ? 1 : 2;
}

static void test_flock(void) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "open");
    int ex = LOCK_EX, sh = LOCK_SH;
    CHECK(flock(fd, LOCK_SH) == 0, "flock SH");
    CHECK(in_child(child_flock_nb, &sh) == 0, "second SH must succeed");
    CHECK(in_child(child_flock_nb, &ex) == 1, "EX vs SH must EWOULDBLOCK");
    CHECK(flock(fd, LOCK_EX) == 0, "upgrade to EX");
    CHECK(in_child(child_flock_nb, &sh) == 1, "SH vs EX must EWOULDBLOCK");
    /* a second open of the same file is a different lock owner */
    int fd2 = open(path, O_RDWR);
    CHECK(flock(fd2, LOCK_SH | LOCK_NB) < 0 && errno == EWOULDBLOCK, "own second open conflicts");
    /* dup shares the lock; closing the dup keeps it */
    int d = dup(fd);
    close(d);
    CHECK(in_child(child_flock_nb, &sh) == 1, "lock survives closing a dup");
    /* fork inherits the open file: child's unlock releases the shared lock */
    pid_t p = fork();
    if (!p) _exit(flock(fd, LOCK_UN) ? 1 : 0);
    int st; waitpid(p, &st, 0);
    CHECK(WIFEXITED(st) && !WEXITSTATUS(st), "child LOCK_UN");
    CHECK(flock(fd2, LOCK_EX | LOCK_NB) == 0, "lock released by child's unlock");
    /* blocking flock waits until another holder exits */
    close(fd2);
    flock(fd, LOCK_UN);
    int pp[2]; pipe(pp);
    p = fork();
    if (!p) { int f = open(path, O_RDWR); flock(f, LOCK_EX); write(pp[1], "x", 1); usleep(150000); _exit(0); }
    char ch; read(pp[0], &ch, 1);
    double t0 = now();
    CHECK(flock(fd, LOCK_EX) == 0, "blocking flock");
    CHECK(now() - t0 > 0.1, "flock did not block (%.3f)", now() - t0);
    waitpid(p, &st, 0);
    close(pp[0]); close(pp[1]);
    CHECK(flock(fd, 99) < 0 && errno == EINVAL, "bad op");
    close(fd);
    CHECK(in_child(child_flock_nb, &ex) == 0, "close releases flock");
}

struct q { off_t s, l; int type; };
static int child_getlk(void *a) {
    struct q *q = a;
    int fd = open(path, O_RDWR);
    struct flock fl = getlk(fd, F_GETLK, q->type, q->s, q->l);
    if (fl.l_type == F_UNLCK) return 0;
    if (fl.l_pid != getppid()) return 3;
    return (int)(10 + fl.l_start);   /* conflicting lock start */
}
static int child_setlk(void *a) {
    struct q *q = a;
    int fd = open(path, O_RDWR);
    if (lk(fd, F_SETLK, q->type, q->s, q->l) == 0) return 0;
    return errno == EAGAIN || errno == EACCES ? 1 : 2;
}

static void test_posix(void) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(write(fd, "0123456789", 10) == 10, "write");
    CHECK(lk(fd, F_SETLK, F_WRLCK, 0, 100) == 0, "WRLCK 0-99");
    CHECK(lk(fd, F_SETLK, F_UNLCK, 40, 20) == 0, "unlock 40-59 (split)");
    struct q a = { 45, 5, F_WRLCK }, b = { 30, 20, F_WRLCK }, c = { 60, 1, F_RDLCK }, d = { 100, 0, F_WRLCK };
    CHECK(in_child(child_setlk, &a) == 0, "hole is free");
    CHECK(in_child(child_getlk, &b) == 10 + 0, "GETLK finds 0-39");
    CHECK(in_child(child_getlk, &c) == 10 + 60, "GETLK finds 60-99");
    CHECK(in_child(child_getlk, &d) == 0, "past end free");
    /* own locks never conflict; F_GETLK by the owner reports unlocked */
    struct flock fl = getlk(fd, F_GETLK, F_WRLCK, 0, 0);
    CHECK(fl.l_type == F_UNLCK, "own GETLK %d", fl.l_type);
    /* re-lock the hole as read and the rest as read: whole range becomes one RDLCK */
    CHECK(lk(fd, F_SETLK, F_RDLCK, 0, 0) == 0, "downgrade to RDLCK whole file");
    struct q r = { 0, 0, F_RDLCK }, w = { 5, 1, F_WRLCK };
    CHECK(in_child(child_setlk, &r) == 0, "RDLCK shared");
    CHECK(in_child(child_setlk, &w) == 1, "WRLCK vs RDLCK fails");
    /* any close of the file releases the process's locks */
    int fd2 = open(path, O_RDONLY);
    close(fd2);
    CHECK(in_child(child_setlk, &w) == 0, "close of another fd released locks");
    /* locks are not inherited across fork */
    CHECK(lk(fd, F_SETLK, F_WRLCK, 0, 10) == 0, "WRLCK");
    pid_t p = fork();
    if (!p) { int f = open(path, O_RDWR); _exit(lk(f, F_SETLK, F_WRLCK, 0, 1) == 0 ? 1 : 0); }
    int st; waitpid(p, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "fork child must not own parent's lock");
    lk(fd, F_SETLK, F_UNLCK, 0, 0);
    /* F_SETLKW blocks until the holder (a child) exits */
    int pp[2]; pipe(pp);
    p = fork();
    if (!p) {
        int f = open(path, O_RDWR);
        lk(f, F_SETLK, F_WRLCK, 0, 5);
        write(pp[1], "x", 1);
        usleep(150000);
        _exit(0);                     /* exit releases */
    }
    char ch; read(pp[0], &ch, 1);
    double t0 = now();
    CHECK(lk(fd, F_SETLKW, F_WRLCK, 2, 1) == 0, "SETLKW");
    CHECK(now() - t0 > 0.1, "SETLKW did not block (%.3f)", now() - t0);
    waitpid(p, &st, 0);
    lk(fd, F_SETLK, F_UNLCK, 0, 0);
    /* deadlock: we hold 0, child holds 1; child waits for 0, then we ask for 1 -> EDEADLK */
    CHECK(lk(fd, F_SETLK, F_WRLCK, 0, 1) == 0, "lock byte 0");
    p = fork();
    if (!p) {
        int f = open(path, O_RDWR);
        lk(f, F_SETLK, F_WRLCK, 1, 1);
        write(pp[1], "y", 1);
        _exit(lk(f, F_SETLKW, F_WRLCK, 0, 1) == 0 ? 0 : 1);   /* blocks until we unlock */
    }
    read(pp[0], &ch, 1);
    usleep(100000);                       /* let the child block */
    errno = 0;
    CHECK(lk(fd, F_SETLKW, F_WRLCK, 1, 1) < 0 && errno == EDEADLK, "EDEADLK expected");
    lk(fd, F_SETLK, F_UNLCK, 0, 1);
    waitpid(p, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "deadlocked child got its lock");
    /* SETLKW interrupted by a signal */
    p = fork();
    if (!p) {
        int f = open(path, O_RDWR);
        lk(f, F_SETLK, F_WRLCK, 0, 0);
        write(pp[1], "z", 1);
        pause();
        _exit(0);
    }
    read(pp[0], &ch, 1);
    struct sigaction sa = { .sa_handler = on_alrm };   /* no SA_RESTART */
    sigaction(SIGALRM, &sa, NULL);
    alarm(1);
    errno = 0;
    CHECK(lk(fd, F_SETLKW, F_WRLCK, 0, 1) < 0 && errno == EINTR, "SETLKW EINTR");
    kill(p, SIGKILL); waitpid(p, &st, 0);
    signal(SIGALRM, SIG_DFL);
    CHECK(lk(fd, F_SETLK, F_WRLCK, 0, 0) == 0, "lock after killed holder");
    /* argument errors */
    struct flock bad = { .l_type = 77, .l_whence = SEEK_SET };
    CHECK(fcntl(fd, F_SETLK, &bad) < 0 && errno == EINVAL, "bad type");
    int ro = open(path, O_RDONLY);
    CHECK(lk(ro, F_SETLK, F_WRLCK, 0, 1) < 0 && errno == EBADF, "WRLCK on O_RDONLY");
    close(ro);                             /* also drops our POSIX locks */
    close(fd);
    close(pp[0]); close(pp[1]);
}

static void test_ofd(void) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    int fd2 = open(path, O_RDWR);
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 10 };
    CHECK(fcntl(fd, F_OFD_SETLK, &fl) == 0, "OFD lock");
    struct flock g = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 5, .l_len = 1 };
    CHECK(fcntl(fd2, F_OFD_GETLK, &g) == 0 && g.l_type == F_WRLCK && g.l_pid == -1 && g.l_start == 0 && g.l_len == 10,
          "OFD GETLK type %d pid %d start %ld len %ld", g.l_type, g.l_pid, (long)g.l_start, (long)g.l_len);
    CHECK(fcntl(fd2, F_OFD_SETLK, &fl) < 0 && errno == EAGAIN, "second open file conflicts (same process)");
    /* a POSIX lock of this very process conflicts with the OFD lock */
    CHECK(lk(fd2, F_SETLK, F_RDLCK, 0, 1) < 0 && errno == EAGAIN, "POSIX vs OFD conflict");
    /* closing another fd does not drop OFD locks (unlike POSIX) */
    int t = open(path, O_RDONLY); close(t);
    CHECK(fcntl(fd2, F_OFD_SETLK, &fl) < 0, "OFD lock survives unrelated close");
    struct flock bad = fl; bad.l_pid = 1;
    CHECK(fcntl(fd2, F_OFD_SETLK, &bad) < 0 && errno == EINVAL, "OFD l_pid != 0");
    /* inherited by fork: the lock belongs to the open file */
    pid_t p = fork();
    if (!p) { struct flock u = fl; u.l_type = F_UNLCK; _exit(fcntl(fd, F_OFD_SETLK, &u) ? 1 : 0); }
    int st; waitpid(p, &st, 0);
    CHECK(fcntl(fd2, F_OFD_SETLK, &fl) == 0, "child unlocked shared OFD lock");
    /* last close releases */
    close(fd2);
    CHECK(fcntl(fd, F_OFD_SETLK, &fl) == 0, "lock after close of holder");
    /* OFD SETLKW blocks until the other open file is closed */
    fd2 = open(path, O_RDWR);
    p = fork();
    if (!p) { usleep(150000); _exit(0); }
    pid_t q = fork();
    if (!q) { usleep(150000); struct flock u = fl; u.l_type = F_UNLCK; fcntl(fd, F_OFD_SETLK, &u); _exit(0); }
    double t0 = now();
    CHECK(fcntl(fd2, F_OFD_SETLKW, &fl) == 0, "OFD SETLKW");
    CHECK(now() - t0 > 0.1, "OFD SETLKW did not block");
    waitpid(p, &st, 0); waitpid(q, &st, 0);
    close(fd); close(fd2);
}

static void test_proc_locks(void) {
    int fd = open(path, O_RDWR);
    flock(fd, LOCK_EX);
    lk(fd, F_SETLK, F_RDLCK, 0, 10);
    char buf[4096] = { 0 };
    int pf = open("/proc/locks", O_RDONLY);
    if (pf >= 0) {
        int n = read(pf, buf, sizeof buf - 1);
        close(pf);
        CHECK(n > 0 && strstr(buf, "FLOCK") && strstr(buf, "POSIX"), "/proc/locks: %s", buf);
    } else CHECK(0, "no /proc/locks");
    close(fd);
}

int main(int argc, char **argv) {
    path = argc > 1 ? argv[1] : "/tmp/locktest.f";
    test_flock(); fprintf(stderr, "locktest: flock done\n");
    test_posix(); fprintf(stderr, "locktest: posix done\n");
    test_ofd();   fprintf(stderr, "locktest: ofd done\n");
    test_proc_locks();
    unlink(path);
    if (fails) { printf("locktest: %d failures\n", fails); return 1; }
    printf("locktest: all passed\n");
    return 0;
}
