/* Eventfd read-side wakeups: large overflow-blocked writers need a wake even
 * when POLLOUT was already true. Run this exact consumer on Linux as an oracle.
 * Thread handshakes bound the exercise; they do not prove kernel enrollment. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { failed++; printf("eventfdwaketest: FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static long long now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec*1000000000+t.tv_nsec; }
struct writer { int fd, entered, done; uint64_t value; ssize_t result; };
static void *put(void *p) {
    struct writer *w=p; __atomic_store_n(&w->entered,1,__ATOMIC_RELEASE);
    w->result=write(w->fd,&w->value,8);
    __atomic_store_n(&w->done,1,__ATOMIC_RELEASE); return NULL;
}
static int until(int *v) {
    long long end=now()+2000000000LL;
    while (!__atomic_load_n(v,__ATOMIC_ACQUIRE) && now()<end) usleep(1000);
    return __atomic_load_n(v,__ATOMIC_ACQUIRE);
}
static int writable(int fd) { struct pollfd p={fd,POLLOUT,0}; return poll(&p,1,0)==1 && (p.revents&POLLOUT); }
static void large_writer(int semaphore) {
    int fd=eventfd(0,semaphore?EFD_SEMAPHORE:0); CHECK(fd>=0); if(fd<0)return;
    uint64_t v=semaphore?UINT64_MAX-1:5;
    CHECK(write(fd,&v,8)==8);
    CHECK(writable(fd)==!semaphore);
    struct writer w={.fd=fd,.value=semaphore?2:UINT64_MAX-4}; pthread_t t;
    int made=pthread_create(&t,NULL,put,&w)==0; CHECK(made); if(!made){close(fd);return;}
    CHECK(until(&w.entered)); usleep(30000);
    CHECK(!__atomic_load_n(&w.done,__ATOMIC_ACQUIRE));
    if(semaphore) {
        /* One free slot is not enough for the writer's two-unit write. */
        CHECK(read(fd,&v,8)==8 && v==1); CHECK(writable(fd)); usleep(30000);
        CHECK(!__atomic_load_n(&w.done,__ATOMIC_ACQUIRE));
        CHECK(read(fd,&v,8)==8 && v==1);
    } else CHECK(read(fd,&v,8)==8 && v==5);
    CHECK(until(&w.done)); CHECK(pthread_join(t,NULL)==0); CHECK(w.result==8);
    if(semaphore) CHECK(!writable(fd));
    else CHECK(read(fd,&v,8)==8 && v==UINT64_MAX-4);
    close(fd);
}
struct poller { int fd, entered, done, result; short events; };
static void *out_wait(void *p) {
    struct poller *w=p; struct pollfd f={w->fd,POLLOUT,0};
    __atomic_store_n(&w->entered,1,__ATOMIC_RELEASE);
    w->result=poll(&f,1,2000); w->events=f.revents;
    __atomic_store_n(&w->done,1,__ATOMIC_RELEASE); return NULL;
}
static void saturated(void) {
    int fd=eventfd(0,EFD_NONBLOCK); CHECK(fd>=0); if(fd<0)return;
    uint64_t v=UINT64_MAX-1; CHECK(write(fd,&v,8)==8); CHECK(!writable(fd));
    v=0; CHECK(write(fd,&v,8)==8); v=1; CHECK(write(fd,&v,8)==-1 && errno==EAGAIN);
    int ep=epoll_create1(EPOLL_CLOEXEC); CHECK(ep>=0);
    struct epoll_event e={.events=EPOLLOUT,.data.fd=fd};
    if(ep>=0) { CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,fd,&e)==0); CHECK(epoll_wait(ep,&e,1,0)==0); }
    struct poller w={.fd=fd}; pthread_t t; int made=pthread_create(&t,NULL,out_wait,&w)==0; CHECK(made);
    if(made) { CHECK(until(&w.entered)); usleep(30000); CHECK(!__atomic_load_n(&w.done,__ATOMIC_ACQUIRE)); }
    CHECK(read(fd,&v,8)==8 && v==UINT64_MAX-1);
    if(made) { CHECK(until(&w.done)); CHECK(pthread_join(t,NULL)==0); CHECK(w.result==1 && (w.events&POLLOUT)); }
    CHECK(writable(fd));
    if(ep>=0) { CHECK(epoll_wait(ep,&e,1,1000)==1 && (e.events&EPOLLOUT)); close(ep); }
    fd_set set; FD_ZERO(&set); FD_SET(fd,&set); struct timeval tv={0,0};
    CHECK(select(fd+1,NULL,&set,NULL,&tv)==1 && FD_ISSET(fd,&set)); close(fd);
}
static void inherited(void) {
    int fd=eventfd(0,0), ready[2]; CHECK(fd>=0); if(fd<0)return;
    int ok=pipe(ready)==0; CHECK(ok); if(!ok){close(fd);return;}
    uint64_t v=9; CHECK(write(fd,&v,8)==8); pid_t p=fork(); CHECK(p>=0);
    if(p==0) { close(ready[0]); char c='w'; if(write(ready[1],&c,1)!=1)_exit(2); v=UINT64_MAX-8; _exit(write(fd,&v,8)==8?0:3); }
    close(ready[1]);
    if(p>0) { char c=0; CHECK(read(ready[0],&c,1)==1 && c=='w'); usleep(30000); CHECK(read(fd,&v,8)==8 && v==9);
        int s=0; CHECK(waitpid(p,&s,0)==p && WIFEXITED(s) && WEXITSTATUS(s)==0);
        CHECK(read(fd,&v,8)==8 && v==UINT64_MAX-8); }
    close(ready[0]); close(fd);
}
int main(void) {
    alarm(25); setvbuf(stdout,NULL,_IONBF,0);
    large_writer(0); large_writer(1); saturated(); inherited();
    alarm(0); printf("eventfdwaketest: %s (%d checks, %d failures)\n",failed?"FAILED":"PASSED",checks,failed); return failed!=0;
}
