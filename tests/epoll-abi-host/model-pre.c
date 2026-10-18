/* Only syscall bodies are production. Scheduling, scan, locks and copies are mocks. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#define nullptr NULL
#define SIGKILL 9
#define SIGSTOP 19
#define SIGBIT(s) (1ull << ((s)-1))
#define BADPTR ((void *)(uintptr_t)1)
struct epoll_event_u { uint32_t events; uint64_t data; } __attribute__((packed));
struct mutex { bool held; };
struct epoll { struct mutex mtx; };
struct file_ops { int unused; };
static const struct file_ops epoll_fops, other_fops={1};
struct file { const struct file_ops *fops; void *priv; };
static struct epoll ep;
static struct file ef={&epoll_fops,&ep}, other={&other_fops,NULL};
static struct { uint64_t sig_mask,saved_mask; bool restore_mask; } thread;
#define current (&thread)
struct ep_scan_ctx { bool took_bkl; int depth; uint64_t filter; };
static bool ready, bkl, scan_bkl;
static uint64_t now, step, wait_ns[8], wait_filter[8], wait_seq[8], masks[8], seq;
static int waits, scans, refs, allocations, copyouts, wait_mode, fault_alloc;
static uint64_t seen_mask;
static int epfd=3;
static struct epoll_event_u out;
static void set_ready(bool yes) { ready=yes; }
static int copy_from_user(void *dst,const void *src,size_t n) {
    if(src==BADPTR || !src) return 1;
    memcpy(dst,src,n); return 0;
}
static int copy_to_user(void *dst,const void *src,size_t n) {
    copyouts++;
    if(dst==BADPTR || !dst) return 1;
    memcpy(dst,src,n); return 0;
}
static void bkl_enter(void) { assert(!bkl); bkl=true; }
static void bkl_exit(void) { assert(bkl); bkl=false; }
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held=true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held=false; }
static struct file *fd_get_ref(int fd) {
    if(fd!=3 && fd!=4) return NULL;
    refs++; return fd==3 ? &ef : &other;
}
static void vfs_close(struct file *f) { assert(f==&ef||f==&other); assert(refs>0); refs--; }
static void *kmalloc(size_t n) { if(fault_alloc) return NULL; allocations++; return malloc(n); }
static void kfree(void *p) { assert(allocations>0); allocations--; free(p); }
static uint64_t time_ns(void) { uint64_t n=now; now+=step; return n; }
static uint64_t poll_seq_read(void) { return ++seq; }
static __attribute__((unused)) uint64_t ep_wait_filter(const struct ep_scan_ctx *x) { return x->filter; }
static int ep_scan_locked(struct epoll *e,struct epoll_event_u *ev,int max,struct ep_scan_ctx *x) {
    assert(e->mtx.held && max>0 && x->depth==1); scans++; seen_mask=current->sig_mask;
    x->filter=0x42;
    if(scan_bkl) { bkl_enter(); x->took_bkl=true; }
    if(ready) { ev[0]=(struct epoll_event_u){1,42}; return 1; }
    return 0;
}
static int poll_wait_seq_masked(uint64_t pseq,uint64_t ns,uint64_t filter) {
    assert(!bkl && !ep.mtx.held && waits<8 && pseq==seq);
    wait_ns[waits]=ns; wait_filter[waits]=filter; wait_seq[waits]=pseq;
    masks[waits]=current->sig_mask; waits++;
    if(wait_mode==1) { now+=ns; return -ETIMEDOUT; }
    if(wait_mode==2) { now+=100; if(waits==1) return 0; }
    return -EINTR;
}
