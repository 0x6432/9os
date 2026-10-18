/* Shared raw-syscall boundary cases; native Linux is reference, not guest proof. */
static int failures, checks;
static void check(const char *name, long got, long want) {
    checks++;
    printf("%s %s got=%ld want=%ld\n", got == want ? "PASS" : "FAIL", name, got, want);
    failures += got != want;
}
static void boundary_cases(void) {
    uint64_t mask = 0;
    struct timespec zero = {0}, negsec = {-1,0}, negns = {0,-1}, billion = {0,1000000000}, maxns = {0,INT64_MAX};
    struct timespec huge = {INT64_MAX,999999999}, wrapzero = {4294967296LL,0}, wrapneg = {2147484,0};
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
