#define _GNU_SOURCE
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
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
static void set_ready(bool yes) { uint64_t v=1; if ((yes ? write(efd,&v,8) : read(efd,&v,8)) != 8) abort(); }
#include "cases.h"
static volatile sig_atomic_t signaled;
static void handler(int s) { (void)s; signaled=1; }
static void interrupted(const char *name,const struct timespec *ts,int ms,bool pwait2) {
    sigset_t old, blocked, after;
    sigemptyset(&blocked); sigaddset(&blocked,SIGALRM);
    if(sigprocmask(SIG_BLOCK,&blocked,&old)) abort();
    struct itimerval timer = {.it_value={0,30000}};
    if(setitimer(ITIMER_REAL,&timer,NULL)) abort();
    signaled=0;
    uint64_t temporary=0;
    check(name,pwait2 ? call2(epfd,&out,1,ts,&temporary,8) : call1(epfd,&out,1,ms,&temporary,8),-EINTR);
    timer.it_value.tv_usec=0; setitimer(ITIMER_REAL,&timer,NULL);
    sigprocmask(SIG_SETMASK,NULL,&after);
    check("interrupt handler ran",signaled,1);
    check("original signal mask restored",sigismember(&after,SIGALRM),1);
    sigprocmask(SIG_SETMASK,&old,NULL);
}
int main(void) {
    epfd=epoll_create1(EPOLL_CLOEXEC); efd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    struct epoll_event ev={.events=EPOLLIN,.data.u64=42};
    if(epfd<0||efd<0||epoll_ctl(epfd,EPOLL_CTL_ADD,efd,&ev)) return 2;
    signal(SIGALRM,handler);
    boundary_cases();
    struct timespec huge={INT64_MAX,999999999}, wrapzero={4294967296LL,0}, wrapneg={2147484,0};
    interrupted("pwait2 huge valid interrupted",&huge,0,true);
    interrupted("pwait2 int-ms-wrap-zero interrupted",&wrapzero,0,true);
    interrupted("pwait2 int-ms-wrap-negative interrupted",&wrapneg,0,true);
    interrupted("pwait2 null timeout interrupted",NULL,0,true);
    interrupted("pwait INT_MAX milliseconds interrupted",NULL,INT32_MAX,false);
    interrupted("pwait negative other than -1 interrupted",NULL,-2,false);
    struct timespec start, finish, shortwait={0,2000001};
    clock_gettime(CLOCK_MONOTONIC,&start);
    check("finite nanosecond timeout",call2(epfd,&out,1,&shortwait,NULL,0),0);
    clock_gettime(CLOCK_MONOTONIC,&finish);
    int64_t elapsed=(finish.tv_sec-start.tv_sec)*1000000000LL+finish.tv_nsec-start.tv_nsec;
    check("timeout cannot expire before requested duration",elapsed>=2000001,1);
    close(efd); close(epfd);
    printf("native Linux reference: %d checks, %d failures (NOT 9os runtime proof)\n",checks,failures);
    return failures ? 1 : 0;
}
