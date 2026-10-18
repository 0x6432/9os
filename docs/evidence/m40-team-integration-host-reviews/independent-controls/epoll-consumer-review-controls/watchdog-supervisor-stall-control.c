/* Standalone public epoll ABI consumer for x86_64/aarch64/riscv64 Linux ABIs.
 * Derived from tests/epoll-abi-host/native.c and cases.h at 6f0fcef.
 * Native Linux runs are reference controls, NOT 9os guest validation.
 * No project headers, generated data, private kernel hooks or extra link deps.
 * The supervisor retains the host fixture's five-second process watchdog;
 * the child alone owns the 30-ms SIGALRM interruption timer.
 */
#define _GNU_SOURCE
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <stddef.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#if !defined(__x86_64__) && !defined(__aarch64__) && !(defined(__riscv) && __riscv_xlen == 64)
#error "epollabitest supports the public x86_64/aarch64/riscv64 64-bit ABIs only"
#endif
_Static_assert(sizeof(long) == 8 && sizeof(void *) == 8, "64-bit syscall ABI required");
/* Old libc headers may not name pwait2 yet; these are public Linux ABI numbers,
 * also used by kernel/arch/{x86_64,aarch64,riscv64}/include/arch/unistd.h. */
#ifndef SYS_epoll_pwait
#ifdef __x86_64__
#define SYS_epoll_pwait 281
#else
#define SYS_epoll_pwait 22
#endif
#endif
#ifndef SYS_epoll_pwait2
#define SYS_epoll_pwait2 441
#endif
/* Raw pwait2 consumes two signed 64-bit fields, independent of libc time_t. */
struct epoll_ts64 { int64_t tv_sec, tv_nsec; };
_Static_assert(sizeof(struct epoll_ts64) == 16, "kernel timespec64 layout");
#ifdef __x86_64__
_Static_assert(sizeof(struct epoll_event) == 12 && offsetof(struct epoll_event, data) == 4,
               "x86_64 packed epoll event ABI");
#else
_Static_assert(sizeof(struct epoll_event) == 16 && offsetof(struct epoll_event, data) == 8,
               "aarch64/riscv64 epoll event ABI");
#endif
#define BADPTR ((void *)(uintptr_t)1)
static int epfd, efd;
static struct epoll_event out;
static long result(long r) { return r < 0 ? -errno : r; }
static long call1(int fd,void *ev,int n,int ms,const void *mask,size_t sz) {
    return result(syscall(SYS_epoll_pwait,fd,ev,n,ms,mask,sz));
}
static long call2(int fd,void *ev,int n,const void *ts,const void *mask,size_t sz) {
    return result(syscall(SYS_epoll_pwait2,fd,ev,n,ts,mask,sz));
}
static void setup_error(const char *name) {
    perror(name);
    _exit(2);
}
static void set_ready(bool yes) {
    uint64_t v=1;
    if ((yes ? write(efd,&v,8) : read(efd,&v,8)) != 8) setup_error("epollabitest: eventfd I/O");
}
/* Same raw syscall boundary cases as the preserved native/model controls. */
static int failures, checks;
static void check(const char *name, long got, long want) {
    checks++;
    printf("%s %s got=%ld want=%ld\n", got == want ? "PASS" : "FAIL", name, got, want);
    failures += got != want;
}
static void boundary_cases(void) {
    uint64_t mask = 0;
    struct epoll_ts64 zero = {0}, negsec = {-1,0}, negns = {0,-1}, billion = {0,1000000000}, maxns = {0,INT64_MAX};
    struct epoll_ts64 huge = {INT64_MAX,999999999}, wrapzero = {4294967296LL,0}, wrapneg = {2147484,0};
    size_t sizes[] = {0,1,7,9,16,128,SIZE_MAX};
    for (unsigned i=0;i<sizeof sizes/sizeof *sizes;i++) {
        char label[96];
        snprintf(label,sizeof label,"pwait nonnull sigset size=%zu",sizes[i]);
        check(label, call1(epfd,&out,1,0,&mask,sizes[i]),-EINVAL);
        snprintf(label,sizeof label,"pwait2 nonnull sigset size=%zu",sizes[i]);
        check(label, call2(epfd,&out,1,&zero,&mask,sizes[i]),-EINVAL);
        snprintf(label,sizeof label,"pwait null sigset ignores size=%zu",sizes[i]);
        check(label, call1(epfd,&out,1,0,NULL,sizes[i]),0);
        snprintf(label,sizeof label,"pwait2 null sigset ignores size=%zu",sizes[i]);
        check(label, call2(epfd,&out,1,&zero,NULL,sizes[i]),0);
    }
    check("pwait inaccessible mask",call1(epfd,&out,1,0,BADPTR,8),-EFAULT);
    check("pwait bad size before inaccessible mask",call1(epfd,&out,1,0,BADPTR,7),-EINVAL);
    check("pwait2 inaccessible timespec",call2(epfd,&out,1,BADPTR,NULL,8),-EFAULT);
    check("pwait2 negative seconds",call2(epfd,&out,1,&negsec,NULL,8),-EINVAL);
    check("pwait2 negative nanoseconds",call2(epfd,&out,1,&negns,NULL,8),-EINVAL);
    check("pwait2 billion nanoseconds",call2(epfd,&out,1,&billion,NULL,8),-EINVAL);
    check("pwait2 INT64_MAX nanoseconds",call2(epfd,&out,1,&maxns,NULL,8),-EINVAL);
    check("pwait2 invalid time before bad fd",call2(-1,&out,1,&negsec,NULL,8),-EINVAL);
    check("pwait2 time copy before mask size",call2(epfd,&out,1,BADPTR,&mask,7),-EFAULT);
    check("pwait sigset before bad fd",call1(-1,&out,1,0,&mask,7),-EINVAL);
    check("pwait sigset before max zero",call1(epfd,&out,0,0,BADPTR,8),-EFAULT);
    check("pwait bad size before max zero",call1(epfd,&out,0,0,BADPTR,7),-EINVAL);
    check("pwait2 sigset before max zero",call2(epfd,&out,0,&zero,BADPTR,8),-EFAULT);
    check("pwait2 bad timespec before max zero",call2(epfd,&out,0,&negsec,BADPTR,8),-EINVAL);
    check("pwait2 inaccessible mask",call2(epfd,&out,1,&zero,BADPTR,8),-EFAULT);
    check("pwait2 bad size before inaccessible mask",call2(epfd,&out,1,&zero,BADPTR,7),-EINVAL);
    struct epoll_ts64 minsec={INT64_MIN,0}, minns={0,INT64_MIN};
    check("pwait2 INT64_MIN seconds",call2(epfd,&out,1,&minsec,NULL,8),-EINVAL);
    check("pwait2 INT64_MIN nanoseconds",call2(epfd,&out,1,&minns,NULL,8),-EINVAL);
    check("pwait valid zero",call1(epfd,&out,1,0,&mask,8),0);
    check("pwait2 valid zero",call2(epfd,&out,1,&zero,&mask,8),0);
    check("pwait max zero",call1(epfd,&out,0,0,NULL,8),-EINVAL);
    check("pwait2 max zero",call2(epfd,&out,0,&zero,NULL,8),-EINVAL);
    check("pwait invalid fd",call1(-1,&out,1,0,NULL,8),-EBADF);
    set_ready(true);
    check("pwait2 invalid timespec even ready",call2(epfd,&out,1,&billion,NULL,8),-EINVAL);
    check("pwait2 huge valid ready",call2(epfd,&out,1,&huge,NULL,8),1);
    check("pwait2 wrap-zero valid ready",call2(epfd,&out,1,&wrapzero,NULL,8),1);
    check("pwait2 wrap-negative valid ready",call2(epfd,&out,1,&wrapneg,NULL,8),1);
    check("pwait2 null timeout ready",call2(epfd,&out,1,NULL,NULL,0),1);
    set_ready(false);
}

static volatile sig_atomic_t signaled;
static void handler(int s) { (void)s; signaled=1; }
static int same_mask(const sigset_t *a,const sigset_t *b) {
    /* The public kernel mask is exactly 64 bits on all three supported ABIs.
     * Compare members, never libc's potentially larger sigset_t padding. */
    for (int sig=1;sig<=64;sig++) {
        int x=sigismember(a,sig), y=sigismember(b,sig);
        if (x<0 || y<0) setup_error("epollabitest: sigismember");
        if (x!=y) return 0;
    }
    return 1;
}
static void interrupted(const char *name,const struct epoll_ts64 *ts,int ms,bool pwait2) {
    sigset_t old, blocked, after, expected;
    sigemptyset(&blocked); sigaddset(&blocked,SIGALRM);
    if(sigprocmask(SIG_BLOCK,&blocked,&old)) setup_error("epollabitest: block alarm");
    expected=old;
    if(sigaddset(&expected,SIGALRM)) setup_error("epollabitest: expected mask");
    struct itimerval timer = {.it_value={0,30000}};
    if(setitimer(ITIMER_REAL,&timer,NULL)) setup_error("epollabitest: arm interrupt timer");
    signaled=0;
    uint64_t temporary=0;
    check(name,pwait2 ? call2(epfd,&out,1,ts,&temporary,8) : call1(epfd,&out,1,ms,&temporary,8),-EINTR);
    timer.it_value.tv_usec=0;
    if(setitimer(ITIMER_REAL,&timer,NULL)) setup_error("epollabitest: cancel interrupt timer");
    if(sigprocmask(SIG_SETMASK,NULL,&after)) setup_error("epollabitest: read mask");
    check("interrupt handler ran",signaled,1);
    check("original signal mask restored",same_mask(&after,&expected),1);
    if(sigprocmask(SIG_SETMASK,&old,NULL)) setup_error("epollabitest: restore mask");
}
static int run_tests(void) {
    epfd=epoll_create1(EPOLL_CLOEXEC); efd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    struct epoll_event ev={.events=EPOLLIN,.data.u64=42};
    if(epfd<0||efd<0||epoll_ctl(epfd,EPOLL_CTL_ADD,efd,&ev)) setup_error("epollabitest: epoll/eventfd setup");
    struct sigaction action={.sa_handler=handler}; /* No SA_RESTART. */
    if(sigemptyset(&action.sa_mask) || sigaction(SIGALRM,&action,NULL)) setup_error("epollabitest: signal handler");
    sigset_t seed, original, expected, after;
    if(sigemptyset(&seed) || sigaddset(&seed,SIGUSR1) || sigprocmask(SIG_BLOCK,&seed,&original))
        setup_error("epollabitest: seed signal mask");
    if(sigprocmask(SIG_SETMASK,NULL,&expected)) setup_error("epollabitest: read seed mask");
    boundary_cases();
    if(sigprocmask(SIG_SETMASK,NULL,&after)) setup_error("epollabitest: read boundary mask");
    check("boundary calls preserve original signal mask",same_mask(&after,&expected),1);
    struct epoll_ts64 huge={INT64_MAX,999999999}, wrapzero={4294967296LL,0}, wrapneg={2147484,0};
    interrupted("pwait2 huge valid interrupted",&huge,0,true);
    interrupted("pwait2 int-ms-wrap-zero interrupted",&wrapzero,0,true);
    interrupted("pwait2 int-ms-wrap-negative interrupted",&wrapneg,0,true);
    interrupted("pwait2 null timeout interrupted",NULL,0,true);
    interrupted("pwait INT_MAX milliseconds interrupted",NULL,INT32_MAX,false);
    interrupted("pwait negative other than -1 interrupted",NULL,-2,false);
    struct timespec start, finish;
    struct epoll_ts64 shortwait={0,2000001};
    uint64_t temporary=0;
    if(clock_gettime(CLOCK_MONOTONIC,&start)) setup_error("epollabitest: start time");
    long finite_result=call2(epfd,&out,1,&shortwait,&temporary,8);
    if(clock_gettime(CLOCK_MONOTONIC,&finish)) setup_error("epollabitest: finish time");
    /* No console I/O inside the measured interval: slow guest serial output
     * must not hide a regressed immediate timeout. */
    check("finite nanosecond timeout",finite_result,0);
    int64_t elapsed=(finish.tv_sec-start.tv_sec)*1000000000LL+finish.tv_nsec-start.tv_nsec;
    check("timeout cannot expire before requested duration",elapsed>=2000001,1);
    if(sigprocmask(SIG_SETMASK,NULL,&after)) setup_error("epollabitest: read timeout mask");
    check("finite timeout preserves original signal mask",same_mask(&after,&expected),1);
    if(sigprocmask(SIG_SETMASK,&original,NULL)) setup_error("epollabitest: restore initial mask");
    close(efd); close(epfd);
    printf("epollabitest: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}

/* The supervisor never shares the child's ITIMER_REAL or temporary signal mask.
 * Polling waitpid(WNOHANG) keeps the same five-second wall-clock watchdog even
 * if a regressed raw syscall ignores a signal or installs an incorrect mask.
 * There is no deadline reset on retries and no longer guest-specific deadline. */
static int monotonic_ns(uint64_t *ns) {
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts)) return -1;
    *ns=(uint64_t)ts.tv_sec*1000000000ULL+(uint64_t)ts.tv_nsec;
    return 0;
}
static void stop_child(pid_t child) {
    if(kill(child,SIGKILL) && errno!=ESRCH) perror("epollabitest: kill child");
    while(waitpid(child,NULL,0)<0 && errno==EINTR) {}
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    uint64_t start, now;
    if(monotonic_ns(&start)) { perror("epollabitest: watchdog clock"); return 2; }
    pid_t child=fork();
    if(child<0) { perror("epollabitest: fork"); return 2; }
    if(child==0) {
        sigset_t all;
        if(sigfillset(&all) || sigprocmask(SIG_SETMASK,&all,NULL)) _exit(2);
        for (;;) pause();
    }
    FILE *pidfile=fopen(getenv("REVIEW_PID_FILE"),"w");
    if(!pidfile) _exit(2);
    fprintf(pidfile,"%ld %ld\n",(long)getpid(),(long)child);
    fclose(pidfile);
    for (;;) pause(); /* REVIEW ONLY: supervisor stalled */
    for (;;) {
        if(monotonic_ns(&now)) {
            perror("epollabitest: watchdog clock"); stop_child(child); return 2;
        }
        if(now-start>=5000000000ULL) {
            fputs("epollabitest: FAIL five-second process watchdog\n",stderr);
            stop_child(child); return 124;
        }
        int status;
        pid_t r=waitpid(child,&status,WNOHANG);
        if(r==child) {
            if(WIFEXITED(status)) return WEXITSTATUS(status);
            fprintf(stderr,"epollabitest: FAIL child signal=%d\n",WIFSIGNALED(status)?WTERMSIG(status):0);
            return 1;
        }
        if(r<0 && errno!=EINTR) {
            perror("epollabitest: waitpid"); stop_child(child); return 2;
        }
        struct timespec nap={0,1000000};
        if(nanosleep(&nap,NULL) && errno!=EINTR) {
            perror("epollabitest: watchdog sleep"); stop_child(child); return 2;
        }
    }
}
