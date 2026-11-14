#define _GNU_SOURCE
/* Caught-signal epoll ABI consumer. Finite waits only: no baseline hang can be
 * excused as an infinite test. SIGUSR1 is a one-shot20ms POSIX timer; SIGALRM
 * remains an independent, fixed5-second process watchdog on every version.
 * Native Linux is reference only, never a 9os guest or startup-cure claim. */
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int checks, failures;
static volatile sig_atomic_t caught, handler_usr1, handler_usr2;
static const char *api_name[] = {"epoll_wait", "epoll_pwait", "epoll_pwait2"};

static void check(const char *name, long got, long want) {
    checks++;
    printf("%s %s got=%ld want=%ld\n", got == want ? "PASS" : "FAIL", name, got, want);
    failures += got != want;
}
static void on_signal(int sig) {
    int saved = errno;
    sigset_t mask;
    (void)sig;
    caught++;
    if (sigprocmask(SIG_SETMASK, NULL, &mask) == 0) {
        handler_usr1 = sigismember(&mask, SIGUSR1);
        handler_usr2 = sigismember(&mask, SIGUSR2);
    }
    errno = saved;
}
static int same_mask(const sigset_t *a, const sigset_t *b) {
    /* sigset_t has libc-specific padding, so compare membership, not bytes. */
    for (int sig = 1; sig < NSIG; sig++)
        if (sigismember(a, sig) != sigismember(b, sig)) return 0;
    return 1;
}
static long wait_call(int api, int epfd, const uint64_t *temporary) {
    struct epoll_event out;
    struct timespec timeout = {0, 100000000};
    if (api == 0) {
#ifdef SYS_epoll_wait
        return syscall(SYS_epoll_wait, epfd, &out, 1, 100);
#else
        /* Architectures without an epoll_wait syscall implement that public
         * API through epoll_pwait with no temporary mask. No numeric guesses. */
        return syscall(SYS_epoll_pwait, epfd, &out, 1, 100, NULL, (size_t)8);
#endif
    }
    if (api == 1)
        return syscall(SYS_epoll_pwait, epfd, &out, 1, 100, temporary, (size_t)8);
    return syscall(SYS_epoll_pwait2, epfd, &out, 1, &timeout, temporary, (size_t)8);
}
static void run_case(int epfd, int api, int restart, int temporary_mask) {
    printf("CASE api=%s restart=%d temporary_mask=%d wait_ms=100 signal_ms=20\n",
           api_name[api], restart, temporary_mask);
    sigset_t original, after, drain;
    sigemptyset(&original);
    sigaddset(&original, SIGUSR2); /* persistent sentinel in both masks */
    if (temporary_mask) sigaddset(&original, SIGUSR1);
    if (sigprocmask(SIG_SETMASK, &original, NULL)) { perror("sigprocmask"); exit(2); }
    struct sigaction action = {.sa_handler=on_signal, .sa_flags=restart ? SA_RESTART : 0};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL)) { perror("sigaction"); exit(2); }
    struct sigevent event = {.sigev_notify=SIGEV_SIGNAL, .sigev_signo=SIGUSR1};
    timer_t timer;
    if (timer_create(CLOCK_MONOTONIC, &event, &timer)) { perror("timer_create"); exit(2); }
    const struct itimerspec once = {.it_value={0,20000000}};
    const struct itimerspec off = {0};
    caught=0; handler_usr1=-1; handler_usr2=-1;
    if (timer_settime(timer, 0, &once, NULL)) { perror("timer_settime"); exit(2); }
    /* The raw Linux sigset ABI is8 bytes on the supported64-bit targets. The
     * temporary mask unblocks SIGUSR1, retains SIGUSR2, and NEVER masks watchdog. */
    uint64_t temporary = UINT64_C(1) << (SIGUSR2-1);
    errno=0;
    long result=wait_call(api, epfd, temporary_mask ? &temporary : NULL);
    int error=errno;
    if (sigprocmask(SIG_SETMASK, NULL, &after)) { perror("get mask"); exit(2); }
    if (timer_settime(timer, 0, &off, NULL) || timer_delete(timer)) { perror("stop timer"); exit(2); }
    check("caught signal returns EINTR even with SA_RESTART", result < 0 ? -error : result, -EINTR);
    check("exactly one handler", caught, 1);
    check("handler blocks its own signal", handler_usr1, 1);
    check("handler retains sentinel mask", handler_usr2, 1);
    check("full original mask restored after sigreturn", same_mask(&original, &after), 1);
    check("original SIGUSR1 membership restored", sigismember(&after, SIGUSR1), temporary_mask);
    check("watchdog remains unblocked", sigismember(&after, SIGALRM), 0);
    /* Discard any late test signal after an already-reported failure, rather
     * than letting it masquerade as the next case's observation. No waits here. */
    sigemptyset(&drain); sigaddset(&drain, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &drain, NULL)) { perror("block drain"); exit(2); }
    struct timespec zero={0};
    while (sigtimedwait(&drain, NULL, &zero) == SIGUSR1) {}
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    sigset_t entry, empty;
    sigemptyset(&empty);
    if (sigprocmask(SIG_SETMASK, &empty, &entry)) { perror("initial mask"); return 2; }
    struct sigaction watchdog={.sa_handler=SIG_DFL};
    sigemptyset(&watchdog.sa_mask);
    if (sigaction(SIGALRM, &watchdog, NULL)) { perror("watchdog action"); return 2; }
    alarm(5); /* Fixed process watchdog; independent of the per-case timer. */
    int epfd=epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("epoll_create1"); return 2; }
    for (int api=0; api<3; api++)
        for (int restart=0; restart<=1; restart++)
            for (int temporary=0; temporary<=(api != 0); temporary++)
                run_case(epfd, api, restart, temporary);
    check("close epoll", close(epfd), 0);
    alarm(0);
    check("restore entry mask", sigprocmask(SIG_SETMASK, &entry, NULL), 0);
    printf("epollrestarttest: %s (%d checks, %d failures; finite100ms, signal20ms, watchdog5s)\n",
           failures ? "FAILED" : "PASSED", checks, failures);
    return failures != 0;
}
