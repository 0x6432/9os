static long call1(int fd,void *ev,int n,int ms,const void *mask,size_t sz) {
    return sys_epoll_pwait(fd,ev,n,ms,mask,sz);
}
static long call2(int fd,void *ev,int n,const void *ts,const void *mask,size_t sz) {
    return sys_epoll_pwait2(fd,ev,n,ts,mask,sz);
}
#include "cases.h"
static void reset(void) {
    assert(!bkl && !ep.mtx.held && !refs && !allocations);
    ready=false; now=123456789; step=0; waits=scans=copyouts=wait_mode=fault_alloc=0;
    scan_bkl=true; current->sig_mask=0xabcdef; current->saved_mask=0; current->restore_mask=false;
    memset(wait_ns,0,sizeof wait_ns);
}
static void duration(const char *name,struct timespec ts,uint64_t expected) {
    reset();
    check(name,call2(epfd,&out,1,&ts,NULL,8),-EINTR);
    check("exact wait ns",(int64_t)wait_ns[0],(int64_t)expected);
    check("exactly one enrollment",waits,1);
}
int main(void) {
    reset(); boundary_cases();
    duration("one nanosecond (no millisecond rounding)",(struct timespec){0,1},1);
    duration("999999 nanoseconds",(struct timespec){0,999999},999999);
    duration("1000001 nanoseconds",(struct timespec){0,1000001},1000001);
    duration("INT_MAX-ms plus 1 ms",(struct timespec){2147483,648000000},2147483648000000ULL);
    duration("int-ms wrap to zero",(struct timespec){4294967296LL,0},4294967296000000000ULL);
    duration("ktime maximum",(struct timespec){9223372036LL,854775807},INT64_MAX-123456789ULL);
    duration("ktime overflow by one ns",(struct timespec){9223372036LL,854775808},INT64_MAX-123456789ULL);
    duration("INT64_MAX seconds",(struct timespec){INT64_MAX,999999999},INT64_MAX-123456789ULL);
    reset();
    check("pwait negative is infinite",call1(epfd,&out,1,-2,NULL,0),-EINTR);
    check("infinite sentinel",(int64_t)wait_ns[0],-1);
    reset();
    check("pwait int max",call1(epfd,&out,1,INT32_MAX,NULL,0),-EINTR);
    check("pwait int max ns",wait_ns[0],2147483647000000ULL);
    reset();
    check("pwait2 NULL is infinite",call2(epfd,&out,1,NULL,NULL,0),-EINTR);
    check("pwait2 infinite sentinel",(int64_t)wait_ns[0],-1);
    reset(); wait_mode=2;
    uint64_t m=UINT64_MAX, stripped=m & ~(SIGBIT(SIGKILL)|SIGBIT(SIGSTOP));
    struct timespec ts={0,500};
    check("spurious wake then EINTR",call2(epfd,&out,1,&ts,&m,8),-EINTR);
    check("original deadline first wait",wait_ns[0],500);
    check("original deadline after wake",wait_ns[1],400);
    check("filter preserved first",wait_filter[0],0x42);
    check("filter preserved second",wait_filter[1],0x42);
    check("epoch resampled",wait_seq[1]-wait_seq[0],1);
    check("temporary mask in scan",seen_mask,stripped);
    check("temporary mask while waiting",masks[0],stripped);
    check("EINTR mask deferred",current->restore_mask,1);
    check("EINTR saved mask",current->saved_mask,0xabcdef);
    reset(); wait_mode=1;
    check("finite timeout",call2(epfd,&out,1,&ts,&m,8),0);
    check("timeout after exact deadline",now,123457289);
    check("timeout mask restored",current->sig_mask,0xabcdef);
    check("no deferred restoration on timeout",current->restore_mask,0);
    reset(); ready=true;
    check("copyout fault",call2(epfd,BADPTR,1,&ts,&m,8),-EFAULT);
    check("copyout fault mask restored",current->sig_mask,0xabcdef);
    reset(); fault_alloc=1;
    check("allocation failure",call2(epfd,&out,1,&ts,&m,8),-ENOMEM);
    check("allocation failure mask untouched",current->sig_mask,0xabcdef);
    reset();
    check("non-epoll fd",call2(4,&out,1,&ts,&m,8),-EINVAL);
    check("non-epoll mask untouched",current->sig_mask,0xabcdef);
    reset();
    check("epoll_wait wrapper",sys_epoll_wait(epfd,&out,1,0),0);
    reset();
    printf("exact-production syscall model: %d checks, %d failures (mock runtime only)\n",checks,failures);
    return failures ? 1 : 0;
}
