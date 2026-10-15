/* An already-zombie child adopted by PID 1 must generate SIGCHLD for its new
 * parent. BusyBox init sleeps in sigtimedwait(), not the kernel child_wait queue.
 * Do not fork/exec extra helpers while observing reaping: they can mask a missed
 * notification by delivering an unrelated SIGCHLD to init. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; printf("orphantest: FAIL %d: %s (errno %d)\n", __LINE__, #c, errno); } } while (0)
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(45);
    for (int i = 0; i < 8; ++i) {
        int fds[2];
        int r = pipe(fds); CHECK(r == 0);
        if (r) break;
        pid_t middle = fork(); CHECK(middle >= 0);
        if (middle == 0) {
            close(fds[0]);
            pid_t grandchild = fork();
            if (grandchild == 0) _exit(42);
            if (grandchild < 0) _exit(98);
            siginfo_t info;
            int got;
            do got = waitid(P_PID, grandchild, &info, WEXITED | WNOWAIT);
            while (got < 0 && errno == EINTR);
            if (got || info.si_pid != grandchild || info.si_code != CLD_EXITED ||
                info.si_status != 42) _exit(99);
            if (write(fds[1], &grandchild, sizeof grandchild) != sizeof grandchild) _exit(97);
            _exit(0); /* Deliberately leave the confirmed zombie for init. */
        }
        close(fds[1]);
        if (middle < 0) { close(fds[0]); break; }
        pid_t orphan = -1;
        ssize_t n;
        do n = read(fds[0], &orphan, sizeof orphan); while (n < 0 && errno == EINTR);
        close(fds[0]);
        CHECK(n == sizeof orphan && orphan > 1);
        int status = 0;
        pid_t got;
        do got = waitpid(middle, &status, 0); while (got < 0 && errno == EINTR);
        CHECK(got == middle && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        if (n != sizeof orphan || orphan <= 1) break;
        char path[64];
        snprintf(path, sizeof path, "/proc/%d/stat", orphan);
        int gone = 0;
        for (int j = 0; j < 500; ++j) {
            /* kill(pid, 0) currently returns ESRCH for a 9os zombie, so it
             * cannot distinguish a retained zombie from an actually reaped PID. */
            int fd = open(path, O_RDONLY);
            if (fd < 0 && errno == ENOENT) { gone = 1; break; }
            if (fd >= 0) close(fd);
            struct timespec ts = { 0, 5000000 };
            nanosleep(&ts, NULL);
        }
        CHECK(gone);
    }
    alarm(0);
    printf("orphantest: %s (%d checks, %d failures)\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails != 0;
}