/* Linux FUTEX_WAKE_OP ABI, real wake lists, shared aliases, COW and atomicity. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define PRIVATE 128
#define ENCODE(op,arg,cmp,val) (((uint32_t)(op)<<28)|((uint32_t)(cmp)<<24)|(((uint32_t)(arg)&4095)<<12)|((uint32_t)(val)&4095))
static int checks, fails;
#define CHECK(x) do { ++checks; if (!(x)) { ++fails; fprintf(stderr,"futexoptest: FAIL %d: %s (errno %d)\n",__LINE__,#x,errno); } } while (0)
static long fx(uint32_t *a,int cmd,uint32_t n,uintptr_t n2,uint32_t *b,uint32_t op) {
    long r=syscall(SYS_futex,a,cmd,n,n2,b,op); return r<0 ? -errno : r;
}
static uint32_t a,b,ready,done;
struct waiter { uint32_t *addr; uint32_t value; long rc; };
static void *waiter(void *arg) {
    struct waiter *w=arg; struct timespec ts={2,0};
    __atomic_add_fetch(&ready,1,__ATOMIC_RELEASE);
    w->rc=fx(w->addr,PRIVATE,w->value,(uintptr_t)&ts,NULL,0);
    __atomic_add_fetch(&done,1,__ATOMIC_RELEASE); return NULL;
}
static void pair_counts(unsigned cmp,int cmpval,int n,int n2,int expected) {
    a=0; b=7; ready=done=0;
    struct waiter w[2]={{&a,0,99},{&b,7,99}}; pthread_t t[2];
    for (int i=0;i<2;i++) CHECK(pthread_create(&t[i],NULL,waiter,&w[i])==0);
    while (__atomic_load_n(&ready,__ATOMIC_ACQUIRE)!=2) usleep(1000);
    usleep(50000);
    CHECK(fx(&a,5|PRIVATE,n,(uintptr_t)n2,&b,ENCODE(1,2,cmp,cmpval))==expected);
    CHECK(b==9); /* comparison must use old 7, not the new 9 */
    if (expected==1) {
        usleep(10000); CHECK(__atomic_load_n(&done,__ATOMIC_ACQUIRE)==1);
        CHECK(fx(&b,1|PRIVATE,1,0,NULL,0)==1);
    }
    for (int i=0;i<2;i++) { CHECK(pthread_join(t[i],NULL)==0); CHECK(w[i].rc==0); }
}
static void pair(unsigned cmp,int cmpval,int expected) { pair_counts(cmp,cmpval,1,1,expected); }
static uint32_t counter;
static void *adder(void *unused) {
    (void)unused;
    for (int i=0;i<2000;i++) {
        if (fx(&a,5|PRIVATE,0,0,&counter,ENCODE(1,1,0,0))<0) return (void *)1;
        __atomic_fetch_add(&counter,1,__ATOMIC_SEQ_CST);
    }
    return NULL;
}
int main(void) {
    alarm(30);
    struct { unsigned op; int arg; uint32_t expected; } ops[]={
        {0,3,3},{1,3,10},{2,8,15},{3,3,4},{4,3,4},{1,-3,4},
        {8|2,5,39},{8|1,32,8},{8|1,-1,0x80000007u}
    };
    for (unsigned i=0;i<sizeof ops/sizeof *ops;i++) {
        b=7; CHECK(fx(&a,5|PRIVATE,0,0,&b,ENCODE(ops[i].op,ops[i].arg,0,0))==0);
        CHECK(b==ops[i].expected);
    }
    pair(0,7,2); pair(0,9,1); pair(1,9,2); pair(2,8,2); pair(3,7,2); pair(4,6,2); pair(5,7,2);
    pair_counts(0,7,0,0,2); pair_counts(0,7,-1,-2,2);
    /* Negative old value/comparand and same-key double wake. */
    a=0; b=(uint32_t)-2; CHECK(fx(&a,5|PRIVATE,0,0,&b,ENCODE(1,-1,2,-1))==0 && b==(uint32_t)-3);
    ready=done=0; a=0;
    struct waiter w[2]={{&a,0,99},{&a,0,99}}; pthread_t t[4];
    for (int i=0;i<2;i++) CHECK(pthread_create(&t[i],NULL,waiter,&w[i])==0);
    while (__atomic_load_n(&ready,__ATOMIC_ACQUIRE)!=2) usleep(1000);
    usleep(50000); CHECK(fx(&a,5|PRIVATE,1,1,&a,ENCODE(2,0,0,0))==2);
    for (int i=0;i<2;i++) { CHECK(pthread_join(t[i],NULL)==0); CHECK(w[i].rc==0); }
    b=7; CHECK(fx(&a,5|PRIVATE,0,0,&b,ENCODE(5,1,0,0))==-ENOSYS && b==7);
    CHECK(fx(&a,5|PRIVATE,0,0,&b,ENCODE(1,2,6,0))==-ENOSYS && b==9);
    CHECK(fx(&a,5|PRIVATE,0,0,(uint32_t *)((char *)&b+1),0)==-EINVAL);
    CHECK(fx(&a,5|PRIVATE,0,0,NULL,0)==-EFAULT);
    CHECK(fx(&a,5|PRIVATE,(uint32_t)-1,0,&b,ENCODE(2,0,0,0))==0);
    uint32_t *ro=mmap(NULL,4096,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(ro!=MAP_FAILED); if (ro!=MAP_FAILED) { CHECK(fx(&a,5|PRIVATE,0,0,ro,0)==-EFAULT); munmap(ro,4096); }
    for (int i=0;i<4;i++) CHECK(pthread_create(&t[i],NULL,adder,NULL)==0);
    for (int i=0;i<4;i++) { void *ret=(void *)1; CHECK(pthread_join(t[i],&ret)==0 && !ret); }
    CHECK(counter==16000);
    /* Shared inode/offset keys must match across different virtual addresses/processes. */
    int fd=memfd_create("wakeop",MFD_CLOEXEC); CHECK(fd>=0 && ftruncate(fd,4096)==0);
    if (fd>=0) {
        uint32_t *x=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        uint32_t *y=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        CHECK(x!=MAP_FAILED && y!=MAP_FAILED && x!=y);
        if (x!=MAP_FAILED && y!=MAP_FAILED) {
            int p[2]; CHECK(pipe(p)==0); pid_t pid=fork(); CHECK(pid>=0);
            if (!pid) { close(p[0]); write(p[1],"R",1); close(p[1]); struct timespec ts={2,0}; _exit(fx(x+1,0,0,(uintptr_t)&ts,NULL,0)==0 ? 0 : 1); }
            if (pid>0) { char c; close(p[1]); CHECK(read(p[0],&c,1)==1); close(p[0]); usleep(50000);
                CHECK(fx(y,5,1,1,y+1,ENCODE(2,0,0,0))==1);
                int st; CHECK(waitpid(pid,&st,0)==pid && WIFEXITED(st) && WEXITSTATUS(st)==0); }
        }
        if (x!=MAP_FAILED) munmap(x,4096); if (y!=MAP_FAILED) munmap(y,4096); close(fd);
    }
    uint32_t *cow=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(cow!=MAP_FAILED);
    if (cow!=MAP_FAILED) {
        *cow=7; int p[2]; CHECK(pipe(p)==0); pid_t pid=fork(); CHECK(pid>=0);
        if (!pid) { char c; close(p[1]); read(p[0],&c,1); close(p[0]); _exit(*cow==7 ? 0 : 1); }
        if (pid>0) { close(p[0]); CHECK(fx(&a,5|PRIVATE,0,0,cow,ENCODE(1,1,0,0))==0 && *cow==8);
            write(p[1],"R",1); close(p[1]); int st; CHECK(waitpid(pid,&st,0)==pid && WIFEXITED(st) && WEXITSTATUS(st)==0); }
        munmap(cow,4096);
    }
    printf("futexoptest: %s (%d checks, %d failures)\n",fails ? "FAILED" : "PASSED",checks,fails);
    return fails!=0;
}
