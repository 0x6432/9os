/* Same-key FUTEX_REQUEUE must count each waiter once, not rotate it repeatedly.
 * Enrollment is proved by moving waiters to a distinct key and back. The exact
 * source is also run on Linux. No fixed settling delay or timing threshold. */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum { FUTEX_WAIT=0, FUTEX_WAKE=1, FUTEX_REQUEUE=3, FUTEX_CMP_REQUEUE=4, FUTEX_PRIVATE_FLAG=128 };
static int checks,failed;
#define CHECK(x) do { checks++;if(!(x)){failed++;printf("futexrequeuetest: FAIL line %d: %s\n",__LINE__,#x);} }while(0)
static long fx(uint32_t *a,int op,int n,int move,uint32_t *b,uint32_t val) {
    long r=syscall(SYS_futex,a,op,n,(void *)(intptr_t)move,b,val);return r<0?-errno:r;
}
static long long ns(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (long long)t.tv_sec*1000000000+t.tv_nsec;}
struct run {uint32_t *a;int flags;int done,errors;};
static void *waiter(void *arg) {
    struct run *r=arg;struct timespec t={8,0};
    long v=syscall(SYS_futex,r->a,FUTEX_WAIT|r->flags,0,&t,NULL,0);
    if(v)__atomic_add_fetch(&r->errors,1,__ATOMIC_RELAXED);
    __atomic_add_fetch(&r->done,1,__ATOMIC_RELEASE);return NULL;
}
static void sample(uint32_t *a,uint32_t *same,uint32_t *other,int flags,int distinct) {
    struct run r={a,flags,0,0};pthread_t t[3];int made=0;
    for(int i=0;i<3;i++){if(pthread_create(&t[i],NULL,waiter,&r))break;made++;}
    CHECK(made==3);long moved=0;long long end=ns()+4000000000LL;
    while(moved<made && ns()<end) {
        long n=fx(a,FUTEX_REQUEUE|flags,0,made-(int)moved,other,0);
        if(n<0){CHECK(n>=0);break;}moved+=n;if(moved<made)usleep(1000);
    }
    CHECK(moved==made);CHECK(fx(other,FUTEX_REQUEUE|flags,0,100,a,0)==made);
    if(moved==made && made==3) {
        CHECK(fx(a,FUTEX_REQUEUE|flags,0,100,same,0)==3);
        CHECK(fx(a,FUTEX_CMP_REQUEUE|flags,0,100,same,0)==3);
        CHECK(fx(a,FUTEX_CMP_REQUEUE|flags,0,100,same,1)==-EAGAIN);
        CHECK(__atomic_load_n(&r.done,__ATOMIC_ACQUIRE)==0);
        if(distinct) {
            /* A mismatching comparison must neither wake nor move anyone. */
            CHECK(fx(a,FUTEX_CMP_REQUEUE|flags,1,100,other,1)==-EAGAIN);
            CHECK(fx(other,FUTEX_WAKE|flags,INT_MAX,0,NULL,0)==0);
            CHECK(fx(a,FUTEX_REQUEUE|flags,0,100,same,0)==3);
            CHECK(fx(a,FUTEX_CMP_REQUEUE|flags,1,100,other,0)==3);
            CHECK(fx(a,FUTEX_WAKE|flags,INT_MAX,0,NULL,0)==0);
            CHECK(fx(other,FUTEX_WAKE|flags,INT_MAX,0,NULL,0)==2);
        } else {
            CHECK(fx(a,FUTEX_CMP_REQUEUE|flags,1,100,same,0)==3);
            CHECK(fx(same,FUTEX_WAKE|flags,INT_MAX,0,NULL,0)==2);
        }
    }
    /* Always release both possible queues after any failed setup. */
    fx(a,FUTEX_WAKE|flags,INT_MAX,0,NULL,0);fx(other,FUTEX_WAKE|flags,INT_MAX,0,NULL,0);
    for(int i=0;i<made;i++)CHECK(pthread_join(t[i],NULL)==0);
    CHECK(r.done==made && r.errors==0);
}
int main(void) {
    alarm(60);setvbuf(stdout,NULL,_IONBF,0);uint32_t words[2]={0};
    for(int distinct=0;distinct<2;distinct++)sample(words,words,words+1,FUTEX_PRIVATE_FLAG,distinct);
    int fd=memfd_create("requeue-alias",MFD_CLOEXEC);CHECK(fd>=0);if(fd<0)return 2;
    CHECK(ftruncate(fd,4096)==0);
    uint32_t *a=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    uint32_t *alias=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    CHECK(a!=MAP_FAILED && alias!=MAP_FAILED && a!=alias);
    if(a!=MAP_FAILED && alias!=MAP_FAILED){for(int distinct=0;distinct<2;distinct++)sample(a,alias,a+1,0,distinct);munmap(a,4096);munmap(alias,4096);}
    close(fd);alarm(0);printf("futexrequeuetest: %s (%d checks, %d failures)\n",failed?"FAILED":"PASSED",checks,failed);return failed!=0;
}
