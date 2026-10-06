/* fd table without the BKL: threads racing open/close/dup/dup2/fcntl on a shared table,
 * fd numbers stay unique (lowest-free allocation under fd_lock), close-on-exec bits follow
 * their slot, and reads on an fd closed by another thread never touch a freed file. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("fdtest: FAIL " __VA_ARGS__); putchar('\n'); fails++; } } while (0)
#define NT 4
#define ITER 3000
static volatile int stop;
static int bad[NT];

static void *churn(void *a) {
    int id = (int)(long)a;
    for (int i = 0; i < ITER; i++) {
        int p[2];
        if (pipe(p)) { bad[id]++; continue; }
        int d = dup(p[0]);
        int e = fcntl(p[1], F_DUPFD_CLOEXEC, 10);
        if (d < 0 || e < 10) bad[id]++;
        if (e >= 0 && !(fcntl(e, F_GETFD) & FD_CLOEXEC)) bad[id]++;
        char c = 'x';
        if (write(e, &c, 1) != 1 || read(d, &c, 1) != 1 || c != 'x') bad[id]++;
        close(p[0]); close(p[1]); close(d); close(e);
    }
    return 0;
}

static int shared;
static void *closer(void *a) {
    (void)a;
    while (!stop) {
        int e = eventfd(1, EFD_NONBLOCK);
        dup2(e, shared);
        close(e);
        close(shared);
    }
    return 0;
}

int main(void) {
    pthread_t t[NT];
    for (int i = 0; i < NT; i++) pthread_create(&t[i], 0, churn, (void *)(long)i);
    for (int i = 0; i < NT; i++) pthread_join(t[i], 0);
    int tb = 0; for (int i = 0; i < NT; i++) tb += bad[i];
    CHECK(tb == 0, "racing pipe/dup/fcntl: %d bad", tb);

    /* every fd closed again: the next allocation is the lowest free slot */
    int a = open("/dev/null", O_RDONLY);
    CHECK(a == 3, "lowest free fd %d", a);
    close(a);

    /* reads/fstat on an fd being closed and replaced by another thread */
    shared = 50;
    pthread_t c; pthread_create(&c, 0, closer, 0);
    int weird = 0;
    for (int i = 0; i < 20000; i++) {
        unsigned long long v;
        ssize_t r = read(shared, &v, 8);
        if (r < 0 && errno != EBADF && errno != EAGAIN) weird++;
        if (r == 8 && v != 1) weird++;
        if (fcntl(shared, F_GETFL) < 0 && errno != EBADF) weird++;
    }
    stop = 1;
    pthread_join(c, 0);
    CHECK(!weird, "read vs close race: %d unexpected", weird);

    /* dup2 onto itself, bad fds, cloexec survives fork */
    CHECK(dup2(1, 1) == 1, "dup2 same");
    CHECK(dup2(999, 5) == -1 && errno == EBADF, "dup2 EBADF");
    CHECK(close(77) == -1 && errno == EBADF, "close EBADF");
    int x = open("/dev/null", O_RDONLY | O_CLOEXEC);
    pid_t pid = fork();
    if (!pid) _exit((fcntl(x, F_GETFD) & FD_CLOEXEC) ? 0 : 1);
    int st; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "cloexec after fork");
    close(x);
    if (fails) return 1;
    puts("fdtest: OK");
    return 0;
}
