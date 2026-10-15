/* vfork completion: fast exit/exec, pending signals and shared-mm lifetime.
 * clone() supplies a separate child stack so delayed children do not overwrite
 * their suspended parent's userspace stack. Also test the real vfork wrapper. */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; printf("vforktest: FAIL %d: %s (errno %d)\n", __LINE__, #c, errno); } } while (0)
struct child_args { volatile sig_atomic_t stage; int mode; pid_t parent; const char *exe; };
static struct child_args args;
static volatile sig_atomic_t signals, early_signal;

static void notified(int sig) {
    (void)sig;
    ++signals;
    if (args.stage != 2) early_signal = 1;
}
static void delay(void) {
    struct timespec ts = { 0, 2000000 };
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}
static int child(void *opaque) {
    struct child_args *a = opaque;
    a->stage = 1;
    if (a->mode == 1) {
        execl(a->exe, a->exe, "--exec-child", (char *)NULL);
        return 99;
    }
    if (a->mode == 2) {
        delay();
        if (kill(a->parent, SIGUSR1)) return 98;
        delay();
    }
    a->stage = 2;
    return 37;
}
static void reap(pid_t pid, int expected) {
    int status = 0;
    pid_t r;
    do r = waitpid(pid, &status, 0); while (r < 0 && errno == EINTR);
    CHECK(r == pid && WIFEXITED(status) && WEXITSTATUS(status) == expected);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--exec-child")) return 17;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(60);
    const size_t stack_size = 128 * 1024;
    char *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(stack != MAP_FAILED);
    if (stack == MAP_FAILED) return 1;
    args.parent = getpid();
    args.exe = strchr(argv[0], '/') ? argv[0] : "/bin/vforktest";
    struct sigaction sa = { .sa_handler = notified };
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    for (int mode = 0; mode < 3; ++mode) {
        int rounds = mode == 0 ? 2000 : mode == 1 ? 256 : 32;
        args.mode = mode;
        for (int i = 0; i < rounds; ++i) {
            args.stage = 0;
            pid_t pid = clone(child, stack + stack_size,
                              CLONE_VM | CLONE_VFORK | SIGCHLD, &args);
            CHECK(pid > 0);
            if (pid <= 0) break;
            CHECK(args.stage == (mode == 1 ? 1 : 2));
            reap(pid, mode == 1 ? 17 : 37);
            if ((i + 1) % 256 == 0)
                printf("vforktest: mode %d completed %d/%d\n", mode, i + 1, rounds);
        }
        printf("vforktest: mode %d complete\n", mode);
    }
    CHECK(signals == 32 && !early_signal);
    for (int i = 0; i < 512; ++i) {
        pid_t pid = vfork();
        if (pid == 0) _exit(23);
        CHECK(pid > 0);
        if (pid <= 0) break;
        reap(pid, 23);
    }
    CHECK(munmap(stack, stack_size) == 0);
    alarm(0);
    printf("vforktest: %s (%d checks, %d failures)\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails != 0;
}