/* Exact Linux bitset ABI oracle, also run unchanged on 9os.
 * Requeue is the enrollment barrier: returned move counts prove all waiters
 * are queued before assertions, without assuming a sleep was long enough. */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define PRIVATE 128
#define ANY UINT32_MAX
static int checks, fails;
#define CHECK(x) do { ++checks; if (!(x)) { ++fails; fprintf(stderr,"futexbitsettest: FAIL %d: %s (errno %d)\n",__LINE__,#x,errno); } } while (0)
static long fx(uint32_t *a,int op,uint32_t n,uintptr_t ts,uint32_t *b,uint32_t mask) {
    long r=syscall(SYS_futex,a,op,n,ts,b,mask); return r<0 ? -errno : r;
}
static int64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (int64_t)ts.tv_sec*1000000000+ts.tv_nsec;
}
static struct timespec deadline(int clock) {
    struct timespec ts; clock_gettime(clock,&ts); ts.tv_sec+=8; return ts;
}
struct waiter { uint32_t *addr, mask; int op; long rc; };
static void *waiter(void *arg) {
    struct waiter *w=arg;
    struct timespec ts=w->op==0 ? (struct timespec){8,0} : deadline(CLOCK_MONOTONIC);
    w->rc=fx(w->addr,w->op|PRIVATE,0,(uintptr_t)&ts,NULL,w->mask); return NULL;
}
static int enrolled(uint32_t *a,uint32_t *b,int n,int flags) {
    int moved=0; int64_t end=now_ns()+4000000000LL;
    while (moved<n && now_ns()<end) {
        long r=fx(a,3|flags,0,n-moved,b,0);
        if (r<0) return 0;
        moved+=(int)r;
        if (moved<n) usleep(1000);
    }
    return moved==n;
}
static void masked_group(void) {
    uint32_t a=0,b=0;
    struct waiter w[5]={{&a,1,9,99},{&a,2,9,99},{&a,4,9,99},{&a,0x80000000u,9,99},{&a,3,9,99}};
    pthread_t t[5]; int made=0;
    for (;made<5;made++) { int r=pthread_create(t+made,NULL,waiter,w+made); CHECK(r==0); if(r)break; }
    int ok=enrolled(&a,&b,made,PRIVATE); CHECK(ok && made==5);
    if(ok) {
        CHECK(fx(&b,10|PRIVATE,INT_MAX,0,NULL,8)==0);
        CHECK(fx(&b,10|PRIVATE,1,0,NULL,1)==1);
        CHECK(fx(&b,10|PRIVATE,INT_MAX,0,NULL,1)==1);
        CHECK(fx(&b,10|PRIVATE,INT_MAX,0,NULL,1)==0);
        CHECK(fx(&b,10|PRIVATE,INT_MAX,0,NULL,2)==1);
        CHECK(fx(&b,10|PRIVATE,1,0,NULL,0x80000000u)==1);
        CHECK(fx(&b,10|PRIVATE,1,0,NULL,0x80000000u)==0);
        /* Plain WAKE matches the remaining low-bit waiter. */
        CHECK(fx(&b,1|PRIVATE,1,0,NULL,0)==1);
        CHECK(fx(&b,10|PRIVATE,INT_MAX,0,NULL,ANY)==0);
    }
    fx(&a,1|PRIVATE,INT_MAX,0,NULL,0);fx(&b,1|PRIVATE,INT_MAX,0,NULL,0);
    for(int i=0;i<made;i++) { CHECK(pthread_join(t[i],NULL)==0);CHECK(w[i].rc==0); }
}
static void plain_wait(void) {
    uint32_t a=0,b=0;
    struct waiter w={&a,0,0,99};pthread_t t;
    int r=pthread_create(&t,NULL,waiter,&w);CHECK(r==0);if(r)return;
    int ok=enrolled(&a,&b,1,PRIVATE);CHECK(ok);
    if(ok)CHECK(fx(&b,10|PRIVATE,1,0,NULL,0x80000000u)==1);
    fx(&a,1|PRIVATE,1,0,NULL,0);fx(&b,1|PRIVATE,1,0,NULL,0);
    CHECK(pthread_join(t,NULL)==0);CHECK(w.rc==0);
}
static void shared_alias(void) {
    int fd=memfd_create("bitset",MFD_CLOEXEC);CHECK(fd>=0);if(fd<0)return;
    int r=ftruncate(fd,4096);CHECK(r==0);if(r){close(fd);return;}
    uint32_t *a=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    uint32_t *b=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    CHECK(a!=MAP_FAILED && b!=MAP_FAILED && a!=b);
    if(a!=MAP_FAILED && b!=MAP_FAILED) {
        pid_t p=fork();CHECK(p>=0);
        if(!p) { struct timespec ts=deadline(CLOCK_MONOTONIC);_exit(fx(a,9,0,(uintptr_t)&ts,NULL,4)==0 ? 0:1); }
        if(p>0) {
            int ok=enrolled(b,b+1,1,0);CHECK(ok);
            if(ok) {
                CHECK(fx(b+1,10,1,0,NULL,2)==0);
                CHECK(fx(b+1,10,1,0,NULL,4)==1);
            }
            fx(b,1,1,0,NULL,0);fx(b+1,1,1,0,NULL,0);
            int st;CHECK(waitpid(p,&st,0)==p && WIFEXITED(st) && WEXITSTATUS(st)==0);
        }
    }
    if(a!=MAP_FAILED)munmap(a,4096);
    if(b!=MAP_FAILED)munmap(b,4096);
    close(fd);
}
int main(void) {
    alarm(30);uint32_t a=0;
    CHECK(fx(&a,9|PRIVATE,0,0,NULL,0)==-EINVAL);
    CHECK(fx(&a,10|PRIVATE,1,0,NULL,0)==-EINVAL);
    CHECK(fx(&a,9|PRIVATE,1,0,NULL,ANY)==-EAGAIN);
    CHECK(fx(&a,10|PRIVATE,1,0,NULL,ANY)==0);
    /* Past absolute deadlines in each supported clock. */
    struct timespec past={0,0};
    CHECK(fx(&a,9|PRIVATE,0,(uintptr_t)&past,NULL,1)==-ETIMEDOUT);
    CHECK(fx(&a,9|PRIVATE|256,0,(uintptr_t)&past,NULL,1)==-ETIMEDOUT);
    masked_group();plain_wait();shared_alias();
    printf("futexbitsettest: %s (%d checks, %d failures)\n",fails?"FAILED":"PASSED",checks,fails);
    return fails?1:0;
}
