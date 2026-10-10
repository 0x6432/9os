/*
 * Signal-driven I/O (O_ASYNC, F_SETOWN/F_GETOWN, F_SETOWN_EX/F_GETOWN_EX, F_SETSIG/F_GETSIG,
 * FIOASYNC, FIOSETOWN/SIOCSPGRP). A file with O_ASYNC sits on one global list; the "kasyncd"
 * kernel thread wakes on every poll_notify(), re-polls those files and, for readiness bits that
 * appeared since its last look, signals the owner: SIGIO by default (plain), or the F_SETSIG
 * signal with siginfo si_code POLL_IN/OUT/ERR/HUP/PRI, si_band = poll mask, si_fd = the
 * descriptor that enabled O_ASYNC. Owners: a thread (F_OWNER_TID), a process (F_OWNER_PID,
 * F_SETOWN pid > 0) or a process group (F_OWNER_PGRP, F_SETOWN pid < 0).
 * Lease breaks (locks.c) use fasync_signal() too, with POLL_MSG.
 */
#include <kernel/vfs.h>
#include <kernel/sched.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/spinlock.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/cpu.h>
#include <kernel/cred.h>
#include <kernel/syscall.h>

#define POLLMSG 0x400
#define POLL_IN 1
#define POLL_OUT 2
#define POLL_MSG 3
#define POLL_ERR 4
#define POLL_PRI 5
#define POLL_HUP 6
#define F_OWNER_TID 0
#define F_OWNER_PID 1
#define F_OWNER_PGRP 2

static spinlock_t async_lock = SPINLOCK_INIT;
static struct list_node async_files = LIST_INIT(async_files);
static int nasync;
static struct wait_queue kasync_wq = WAIT_QUEUE_INIT(kasync_wq);
static int kasyncd_started;

struct pgrp_ctx { int pgid; const struct ksiginfo *ki; };
static void pgrp_fn(struct process *p, void *c) {
    struct pgrp_ctx *x = c;
    if (p->pgid == x->pgid && p->state != P_ZOMBIE) signal_send_info(p, x->ki);
}

/* signal f's owner about events (code = POLL_*); caller holds the BKL */
void fasync_signal(struct file *f, int code, unsigned band) {
    int type = __atomic_load_n(&f->own_type, __ATOMIC_RELAXED), pid = __atomic_load_n(&f->own_pid, __ATOMIC_RELAXED);
    if (!pid) return;
    struct ksiginfo ki;
    memset(&ki, 0, sizeof ki);
    ki.signo = f->sig ? f->sig : SIGIO;
    if (f->sig) { ki.code = code; ki.v = band; ki.i1 = f->async_fd; ki._pad = KSI_POLL; }
    else ki.code = SI_KERNEL;
    if (type == F_OWNER_PGRP) {
        struct pgrp_ctx x = { pid, &ki };
        process_list(pgrp_fn, &x);
    } else if (type == F_OWNER_TID) {
        struct thread *t = process_find_thread(pid);
        if (t && t->proc && t->proc->state != P_ZOMBIE) signal_thread_info(t, &ki);
    } else {
        struct process *p = process_find(pid);
        if (p && p->state != P_ZOMBIE) signal_send_info(p, &ki);
    }
}

bool file_get_live(struct file *f) {
    int r = __atomic_load_n(&f->refcount, __ATOMIC_RELAXED);
    while (r > 0) if (__atomic_compare_exchange_n(&f->refcount, &r, r + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return true;
    return false;
}

static void scan(void) {
    enum { MAXF = 64 };
    struct file *fs[MAXF];
    int n = 0;
    uint64_t fl = spin_lock_irqsave(&async_lock);
    list_for_each(it, &async_files) {
        struct file *f = list_entry(it, struct file, async_node);
        if (n < MAXF && file_get_live(f)) fs[n++] = f;
    }
    spin_unlock_irqrestore(&async_lock, fl);
    bkl_enter();
    for (int k = 0; k < n; k++) {
        struct file *f = fs[k];
        unsigned m = f->fops && f->fops->poll ? f->fops->poll(f) : POLLIN | POLLOUT;
        unsigned old = __atomic_exchange_n(&f->async_last, m, __ATOMIC_RELAXED), nw = m & ~old;
        if (!(f->flags & O_ASYNC) || !nw) continue;
        int code = nw & POLLERR ? POLL_ERR : nw & POLLHUP ? POLL_HUP : nw & POLLPRI ? POLL_PRI : nw & POLLIN ? POLL_IN : POLL_OUT;
        unsigned band = m & (POLLIN | POLLPRI | POLLOUT | POLLERR | POLLHUP);
        if (band & POLLIN) band |= POLLRDNORM;
        if (band & POLLOUT) band |= POLLWRNORM;
        fasync_signal(f, code, band);
    }
    bkl_exit();
    for (int k = 0; k < n; k++) vfs_close(fs[k]);
}

static void kasync_main(void *arg) {
    (void)arg;
    for (;;) {
        wait_until_sl(&kasync_wq, __atomic_load_n(&nasync, __ATOMIC_ACQUIRE) > 0);
        uint64_t seq = poll_seq_read();
        scan();
        if (__atomic_load_n(&nasync, __ATOMIC_ACQUIRE) > 0) poll_wait_seq(seq, UINT64_MAX);
    }
}

/* O_ASYNC on/off for f (fd = the descriptor used, reported as si_fd) */
void fasync_set(struct file *f, int fd, bool on) {
    unsigned m0 = 0;
    if (on && !f->on_async) {                  /* what is ready now never signals */
        bool took = !bkl_held();
        if (took) bkl_enter();
        m0 = f->fops && f->fops->poll ? f->fops->poll(f) : POLLIN | POLLOUT;
        if (took) bkl_exit();
    }
    uint64_t fl = spin_lock_irqsave(&async_lock);
    if (on) {
        f->async_fd = fd;
        if (!f->on_async) {
            f->on_async = true;
            f->async_last = m0;
            list_add_tail(&async_files, &f->async_node);
            nasync++;
        }
        __atomic_fetch_or(&f->flags, O_ASYNC, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_and(&f->flags, ~(uint32_t)O_ASYNC, __ATOMIC_RELAXED);
        if (f->on_async) { list_del(&f->async_node); f->on_async = false; nasync--; }
    }
    spin_unlock_irqrestore(&async_lock, fl);
    if (on) {
        if (!__atomic_exchange_n(&kasyncd_started, 1, __ATOMIC_ACQ_REL)) thread_create("kasyncd", kasync_main, nullptr);
        wake_up(&kasync_wq);
    }
}

/* last reference of f */
void fasync_release(struct file *f) { if (f->on_async) fasync_set(f, 0, false); }

int fasync_setown(struct file *f, int type, int pid) {
    if (type < F_OWNER_TID || type > F_OWNER_PGRP) return -EINVAL;
    if (pid < 0) return -EINVAL;
    if (pid) {
        if (type == F_OWNER_TID ? !process_find_thread(pid) : type == F_OWNER_PID ? !process_find(pid) : false) return -ESRCH;
    }
    __atomic_store_n(&f->own_pid, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&f->own_type, type, __ATOMIC_RELAXED);
    __atomic_store_n(&f->own_pid, pid, __ATOMIC_RELAXED);
    return 0;
}
/* F_GETOWN: pid, or -pgid */
int fasync_getown(struct file *f) { return f->own_type == F_OWNER_PGRP ? -f->own_pid : f->own_pid; }
