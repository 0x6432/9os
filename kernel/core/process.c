/* Processes: creation, fork/clone, exit, wait. */
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <kernel/mm.h>
#include <kernel/vfs.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/tty.h>
#include <kernel/exec.h>
#include <kernel/arch.h>
#include <kernel/time.h>
#include <arch/syscall.h>

#define CLONE_VM 0x100
#define CLONE_FS 0x200
#define CLONE_FILES 0x400
#define CLONE_SIGHAND 0x800
#define CLONE_VFORK 0x4000
#define CLONE_PARENT 0x8000
#define CLONE_THREAD 0x10000
#define CLONE_SETTLS 0x80000
#define CLONE_PARENT_SETTID 0x100000
#define CLONE_CHILD_CLEARTID 0x200000
#define CLONE_CHILD_SETTID 0x1000000
#define CLONE_PIDFD 0x1000
#define CLONE_UNTRACED 0x800000
#define CLONE_CLEAR_SIGHAND 0x100000000ULL

/* ptrace.c */
void ptrace_fork_attach(struct thread *child, uint64_t flags, int exit_signal);
void ptrace_fork_event(uint64_t flags, int exit_signal, int child_tid);
void ptrace_exit_event(int status);
void ptrace_thread_gone(struct thread *t, struct process *p, int status);
void ptrace_tracer_exit(struct process *p);
int ptrace_wait(struct process *self, int idtype, int id, int options, struct wait_result *res);
bool ptrace_has_tracees(struct process *self, int idtype, int id);
/* posix timers (sys_timer.c), pidfd (anonfd.c) */
void posix_timers_exit(struct process *p);
int pidfd_create(struct process *p, int flags);

static struct list_node all_procs = LIST_INIT(all_procs);
static struct process *init_proc;

struct trap_frame *thread_user_frame(struct thread *t);
void arch_thread_init_user(struct thread *t);
void arch_thread_copy_fpu(struct thread *dst, struct thread *src);
void arch_set_tls(struct thread *t, uint64_t v);
int futex_wake(uint32_t *uaddr, int n);

struct process *process_find(int pid) {
    list_for_each(it, &all_procs) {
        struct process *p = list_entry(it, struct process, all_node);
        if (p->pid == pid) return p;
    }
    return nullptr;
}

/* Linux scheduling syscalls take a TID, not necessarily a process leader's PID.
 * Caller holds the BKL so the process/thread lists and returned pointer stay live. */
struct thread *process_find_thread(int tid) {
    list_for_each(it, &all_procs) {
        struct process *p = list_entry(it, struct process, all_node);
        list_for_each(ti, &p->threads) {
            struct thread *t = list_entry(ti, struct thread, proc_node);
            if (t->tid == tid) return t;
        }
    }
    return nullptr;
}

void process_list(void (*fn)(struct process *, void *), void *ctx) {
    list_for_each_safe(it, tmp, &all_procs) fn(list_entry(it, struct process, all_node), ctx);
}

static const struct lock_class fd_class = { "fd_table", LR_FD, false };
static struct process *proc_alloc(void) {
    struct process *p = kzalloc(sizeof *p);
    if (!p) return nullptr;
    spin_lock_init_class(&p->fd_lock, &fd_class);
    list_init(&p->children);
    list_init(&p->sibling);
    list_init(&p->threads);
    list_init(&p->timers);
    list_init(&p->tracees);
    list_init(&p->pt_exits);
    list_init(&p->sigq.rtq);
    wait_queue_init(&p->child_wait);
    p->umask = 022;
    p->exit_signal = SIGCHLD;
    list_add_tail(&all_procs, &p->all_node);
    return p;
}

static void init_thread_entry(void *arg) {
    static char *argv[] = { "/sbin/init", nullptr, nullptr };
    static char *envp[] = { "HOME=/root", "PATH=/sbin:/usr/sbin:/bin:/usr/bin", "TERM=linux", "SHELL=/bin/sh", nullptr };
    struct trap_frame *f = thread_user_frame(current);
    const char *candidates[] = { (const char *)arg, "/sbin/init", "/init", "/bin/sh" };
    struct process *p = curproc;
    /* stdin/stdout/stderr on the console */
    struct file *con;
    if (!vfs_open("/dev/console", O_RDWR, 0, &con)) {
        p->fds[0] = con; p->fds[1] = file_get(con); p->fds[2] = file_get(con);
    }
    for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
        if (!candidates[i] || !*candidates[i]) continue;
        argv[0] = (char *)candidates[i];
        int r = do_execve(candidates[i], argv, envp, f);
        if (!r) {
            pr_info("init: started %s (pid %d)\n", candidates[i], p->pid);
            arch_irq_disable();
            bkl_exit();          /* leaving the kernel for user mode */
            __asm__ volatile("" ::: "memory");
            extern void arch_enter_user(struct trap_frame *f) __attribute__((noreturn));
            arch_enter_user(f);
        }
        pr_warn("init: exec %s failed (%d)\n", candidates[i], r);
    }
    panic("no init found");
}

struct process *process_create_init(const char *path) {
    struct process *p = proc_alloc();
    struct thread *t = thread_alloc("init");
    p->pid = p->pgid = p->sid = t->tid;
    p->root = vfs_root; iget(vfs_root);
    p->cwd = vfs_root; iget(vfs_root);
    p->mm = mm_create();
    strlcpy(p->name, "init", sizeof p->name);
    p->cred = cred_get(&init_cred);
    t->proc = p;
    list_add_tail(&p->threads, &t->proc_node);
    init_proc = p;
    arch_thread_init(t, init_thread_entry, (void *)path);
    thread_start(t);
    return p;
}

int process_fork(struct trap_frame *f, uint64_t flags, uint64_t newsp, int *ptid, int *ctid, uint64_t tls) {
    return process_fork_ex(f, flags & ~0xffULL, newsp, ptid, ctid, tls, (int)(flags & 0xff),
                           (flags & CLONE_PIDFD) ? ptid : nullptr);
}

int process_fork_ex(struct trap_frame *f, uint64_t flags, uint64_t newsp, int *ptid, int *ctid, uint64_t tls,
                    int exit_signal, int *upidfd) {
    struct process *parent = curproc;
    if (exit_signal < 0 || exit_signal >= NSIG) return -EINVAL;
    if ((flags & CLONE_PIDFD) && (flags & CLONE_THREAD)) return -EINVAL;
    if ((flags & CLONE_CLEAR_SIGHAND) && (flags & CLONE_SIGHAND)) return -EINVAL;
    struct thread *t = thread_alloc(current->name);
    if (!t) return -ENOMEM;
    struct process *p;
    if (flags & CLONE_THREAD) {
        p = parent;
    } else {
        p = proc_alloc();
        if (!p) { thread_free(t); return -ENOMEM; }
        p->pid = t->tid;
        p->pgid = parent->pgid; p->sid = parent->sid;
        p->ctty = parent->ctty;
        p->umask = parent->umask;
        p->cred = cred_get(current->cred);
        strlcpy(p->name, parent->name, sizeof p->name);
        if (parent->cmdline) { p->cmdline = kmalloc(parent->cmdline_len + 1); memcpy(p->cmdline, parent->cmdline, parent->cmdline_len); p->cmdline_len = parent->cmdline_len; }
        if (parent->exe) p->exe = strdup(parent->exe);
        p->start_ticks = jiffies;
        memcpy(p->sigactions, parent->sigactions, sizeof p->sigactions);
        if (flags & CLONE_CLEAR_SIGHAND)
            for (int i = 1; i < NSIG; i++) if (p->sigactions[i].handler != SIG_IGN) p->sigactions[i].handler = SIG_DFL;
        p->exit_signal = exit_signal;
        if (flags & CLONE_VM) { p->mm = parent->mm; __atomic_add_fetch(&p->mm->refcount, 1, __ATOMIC_RELAXED); }
        else {
            p->mm = mm_clone(parent->mm);
            if (!p->mm) { list_del(&p->all_node); cred_put(p->cred); kfree(p); thread_free(t); return -ENOMEM; }
        }
        {   /* sibling threads may close/dup without the BKL */
            uint64_t fl = spin_lock_irqsave(&parent->fd_lock);
            for (int i = 0; i < MAX_FDS; i++) if (parent->fds[i]) p->fds[i] = file_get(parent->fds[i]);
            memcpy(p->cloexec, parent->cloexec, sizeof p->cloexec);
            spin_unlock_irqrestore(&parent->fd_lock, fl);
        }
        vfs_ns_lock();                    /* cwd/root change under the namespace mutex */
        p->cwd = parent->cwd; iget(p->cwd);
        p->root = parent->root; iget(p->root);
        vfs_ns_unlock();
        struct process *pp = (flags & CLONE_PARENT) && parent->parent ? parent->parent : parent;
        p->parent = pp;
        list_add_tail(&pp->children, &p->sibling);
    }
    t->proc = p;
    t->sig_mask = current->sig_mask;
    list_add_tail(&p->threads, &t->proc_node);

    struct trap_frame *cf = thread_user_frame(t);
    *cf = *f;
    SC_SET_RET(cf, 0);
    if (newsp) FRAME_SP(cf) = newsp;
    arch_thread_init_user(t);
    arch_thread_copy_fpu(t, current);
    if (flags & CLONE_SETTLS) arch_set_tls(t, tls);
    if ((flags & CLONE_PARENT_SETTID) && ptid) copy_to_user(ptid, &t->tid, sizeof(int));
    if ((flags & CLONE_CHILD_SETTID) && ctid) mm_write(p->mm, (vaddr_t)ctid, &t->tid, sizeof(int));
    if (flags & CLONE_CHILD_CLEARTID) t->clear_child_tid = ctid;
    if (upidfd) {
        int pfd = pidfd_create(p, 0);
        if (pfd >= 0 && copy_to_user(upidfd, &pfd, sizeof pfd)) pfd = -EFAULT;
        if (pfd < 0) pr_warn("clone: pidfd for %d failed (%d)\n", p->pid, pfd);
    }
    if (current->ptracer && !(flags & CLONE_UNTRACED)) ptrace_fork_attach(t, flags, exit_signal);

    struct vfork_done vd;
    if (flags & CLONE_VFORK) {
        vd.done = false;
        wait_queue_init(&vd.wq);
        p->vfork = &vd;
    }
    int tid = t->tid;
    thread_start(t);
    if (current->ptracer && !(flags & CLONE_UNTRACED)) ptrace_fork_event(flags, exit_signal, tid);
    if (flags & CLONE_VFORK) {
        while (!vd.done) wait_event(&vd.wq);
    }
    return tid;
}

void signal_send_internal_chld(struct process *parent, struct process *child);

static void reparent_children(struct process *p) {
    list_for_each_safe(it, tmp, &p->children) {
        struct process *c = list_entry(it, struct process, sibling);
        list_del(&c->sibling);
        c->parent = init_proc;
        list_add_tail(&init_proc->children, &c->sibling);
        if (c->state == P_ZOMBIE) wake_up(&init_proc->child_wait);
    }
}

/* robust futexes: mark futexes still held by the dying thread OWNER_DIED and wake a waiter */
#define FUTEX_WAITERS 0x80000000u
#define FUTEX_OWNER_DIED 0x40000000u
#define FUTEX_TID_MASK 0x3fffffffu
static void robust_futex_death(struct thread *t, uint64_t uaddr) {
    uint32_t v;
    if (uaddr & 3) return;
    /* the BKL serialises kernel writers; a user-space CAS racing with this sees the new word */
    if (copy_from_user(&v, (void *)uaddr, 4)) return;
    if ((v & FUTEX_TID_MASK) != (uint32_t)t->tid) return;
    uint32_t nv = (v & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
    if (copy_to_user((void *)uaddr, &nv, 4)) return;
    futex_wake((uint32_t *)uaddr, 1);
}
void robust_list_exit(struct thread *t) {
    uint64_t head = t->robust_list;
    t->robust_list = 0;
    if (!head || !t->proc || !t->proc->mm) return;
    uint64_t h[3];                      /* struct robust_list_head: list.next, futex_offset, list_op_pending */
    if (copy_from_user(h, (void *)head, sizeof h)) return;
    int64_t off = (int64_t)h[1];
    uint64_t entry = h[0] & ~1ULL, pending = h[2] & ~1ULL;
    for (int n = 0; entry && entry != head && n < 2048; n++) {
        uint64_t next;
        if (copy_from_user(&next, (void *)entry, 8)) break;
        if (entry != pending) robust_futex_death(t, entry + off);
        entry = next & ~1ULL;
    }
    if (pending) robust_futex_death(t, pending + off);
}

static void release_thread_tid(struct thread *t) {
    robust_list_exit(t);
    if (t->clear_child_tid && t->proc && t->proc->mm) {
        int zero = 0;
        if (!copy_to_user(t->clear_child_tid, &zero, sizeof zero)) futex_wake((uint32_t *)t->clear_child_tid, 1);
    }
}

__noreturn void thread_exit_only(void) {
    fd_borrow_release();
    release_thread_tid(current);
    if (current->ptracer) ptrace_thread_gone(current, curproc, 0);
    arch_irq_disable();
    list_del(&current->proc_node);
    thread_exit();
}

__noreturn void process_exit(int status) {
    struct process *p = curproc;
    if (p == init_proc) panic("init exited with status %x", status);
    fd_borrow_release();
    if (current->ptracer) ptrace_exit_event(status);     /* PTRACE_O_TRACEEXIT stop */
    /* other threads of this process are killed on their next return to user mode */
    list_for_each(it, &p->threads) {
        struct thread *t = list_entry(it, struct thread, proc_node);
        if (t != current) { t->killed = true; thread_wake(t); }
    }
    release_thread_tid(current);
    posix_timers_exit(p);
    ptrace_tracer_exit(p);
    for (int i = 0; i < MAX_FDS; i++) if (p->fds[i]) vfs_close(fd_slot_set(p, i, nullptr));
    vfs_ns_lock();
    iput(p->cwd); iput(p->root);
    p->cwd = p->root = nullptr;
    vfs_ns_unlock();
    if (p->ctty && p->sid == p->pid) { p->ctty->sid = 0; p->ctty->pgrp = 0; }
    vmm_switch(kernel_pt);
    struct mm *mm = p->mm;
    p->mm = nullptr;
    arch_irq_disable();
    list_del(&current->proc_node);
    current->proc = nullptr;
    arch_irq_enable();
    mm_put(mm);
    reparent_children(p);
    p->exit_status = status;
    p->state = P_ZOMBIE;
    if (current->ptracer) ptrace_thread_gone(current, p, status);
    if (p->vfork) { p->vfork->done = true; wake_up(&p->vfork->wq); p->vfork = nullptr; }
    struct process *parent = p->parent;
    if (parent) {
        int code = (status & 0x7f) == 0 ? CLD_EXITED : (status & 0x80) ? CLD_DUMPED : CLD_KILLED;
        signal_send_chld(parent, p, code, code == CLD_EXITED ? (status >> 8) & 0xff : status & 0x7f);
        wake_up(&parent->child_wait);
    }
    poll_notify();                     /* pidfd pollers */
    thread_exit();
}

#define WNOHANG 1
#define WUNTRACED 2
#define WEXITED 4
#define WCONTINUED 8
#define WNOWAIT 0x01000000
#define __WALL 0x40000000
#define __WCLONE 0x80000000

/* idtype: P_ALL 0, P_PID 1, P_PGID 2 (id 0 = caller's group) */
static bool child_matches(struct process *c, int idtype, int id, int options, struct process *self) {
    if (idtype == 1 && c->pid != id) return false;
    if (idtype == 2 && c->pgid != (id ? id : self->pgid)) return false;
    /* "clone" children (exit signal other than SIGCHLD) need __WCLONE or __WALL */
    bool clone_child = c->exit_signal != SIGCHLD;
    if (!(options & __WALL) && clone_child != !!(options & __WCLONE)) return false;
    return true;
}

static void proc_free(struct process *c) {
    kfree(c->cmdline); kfree(c->exe);
    cred_put(c->cred);
    sigq_flush_proc(c);
    kfree(c);
}
void process_put(struct process *p) {
    if (--p->refs == 0 && p->reaped) proc_free(p);
}

static void reap(struct process *self, struct process *c) {
    struct rusage_k cru = { c->utime_ns + c->cutime_ns, c->stime_ns + c->cstime_ns,
                            c->min_flt + c->cmin_flt, c->nvcsw + c->cnvcsw, c->nivcsw + c->cnivcsw };
    self->cutime_ns += cru.utime_ns; self->cstime_ns += cru.stime_ns;
    self->cmin_flt += cru.min_flt; self->cnvcsw += cru.nvcsw; self->cnivcsw += cru.nivcsw;
    current->reaped_ru = cru;
    list_del(&c->sibling);
    list_del(&c->all_node);
    c->reaped = true;
    if (!c->refs) proc_free(c);
}

static void fill_result(struct wait_result *r, struct process *c, int code, int status) {
    r->pid = c->pid;
    r->status = status;
    r->code = code;
    r->uid = c->cred ? c->cred->uid : 0;
    r->utime = c->utime_ns / 10000000;
    r->stime = c->stime_ns / 10000000;
}

/* Returns the pid reported (res filled), 0 for WNOHANG without a candidate, or -errno. */
int64_t do_wait_ex(int idtype, int id, int options, struct wait_result *res) {
    struct process *self = curproc;
    uint64_t irqf = arch_irq_save();
    int64_t ret;
    bool nowait = options & WNOWAIT;
    for (;;) {
        bool any = false;
        /* ptrace stops and exits of tracees come first (ptrace.c) */
        int tr = ptrace_wait(self, idtype, id, options, res);
        if (tr) { ret = tr; goto out; }
        any = ptrace_has_tracees(self, idtype, id);
        list_for_each_safe(it, tmp, &self->children) {
            struct process *c = list_entry(it, struct process, sibling);
            if (!child_matches(c, idtype, id, options, self)) continue;
            any = true;
            if (c->state == P_ZOMBIE) {
                if (!(options & WEXITED)) continue;
                int st = c->exit_status;
                int code = (st & 0x7f) == 0 ? CLD_EXITED : (st & 0x80) ? CLD_DUMPED : CLD_KILLED;
                fill_result(res, c, code, st);
                ret = c->pid;
                current->reaped_ru = (struct rusage_k){ c->utime_ns + c->cutime_ns, c->stime_ns + c->cstime_ns,
                                        c->min_flt + c->cmin_flt, c->nvcsw + c->cnvcsw, c->nivcsw + c->cnivcsw };
                if (!nowait) reap(self, c);
                goto out;
            }
            if ((options & WUNTRACED) && c->stopped && !c->stop_reported) {
                if (!nowait) c->stop_reported = true;
                fill_result(res, c, CLD_STOPPED, (c->stop_sig << 8) | 0x7f);
                ret = c->pid;
                goto out;
            }
            if ((options & WCONTINUED) && c->cont_reported) {
                if (!nowait) c->cont_reported = false;
                fill_result(res, c, CLD_CONTINUED, 0xffff);
                ret = c->pid;
                goto out;
            }
        }
        if (!any) { ret = -ECHILD; goto out; }
        if (options & WNOHANG) { ret = 0; goto out; }
        int r = wait_event(&self->child_wait);
        if (r) { ret = r; goto out; }
    }
out:
    arch_irq_restore(irqf);
    return ret;
}

int64_t do_wait(int pid, int *ustatus, int options, int *out_pid) {
    int idtype = pid > 0 ? 1 : pid == -1 ? 0 : 2;
    int id = pid > 0 ? pid : pid == 0 ? 0 : -pid;
    struct wait_result res;
    int64_t r = do_wait_ex(idtype, id, (options & ~WNOWAIT) | WEXITED, &res);
    if (r > 0 && ustatus && copy_to_user(ustatus, &res.status, sizeof res.status)) return -EFAULT;
    return r;
}
