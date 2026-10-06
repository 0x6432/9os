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
    wait_queue_init(&p->child_wait);
    p->umask = 022;
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
    t->proc = p;
    list_add_tail(&p->threads, &t->proc_node);
    init_proc = p;
    arch_thread_init(t, init_thread_entry, (void *)path);
    thread_start(t);
    return p;
}

int process_fork(struct trap_frame *f, uint64_t flags, uint64_t newsp, int *ptid, int *ctid, uint64_t tls) {
    struct process *parent = curproc;
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
        p->uid = parent->uid; p->gid = parent->gid; p->euid = parent->euid; p->egid = parent->egid;
        strlcpy(p->name, parent->name, sizeof p->name);
        if (parent->cmdline) { p->cmdline = kmalloc(parent->cmdline_len + 1); memcpy(p->cmdline, parent->cmdline, parent->cmdline_len); p->cmdline_len = parent->cmdline_len; }
        if (parent->exe) p->exe = strdup(parent->exe);
        p->start_ticks = jiffies;
        memcpy(p->sigactions, parent->sigactions, sizeof p->sigactions);
        if (flags & CLONE_VM) { p->mm = parent->mm; __atomic_add_fetch(&p->mm->refcount, 1, __ATOMIC_RELAXED); }
        else {
            p->mm = mm_clone(parent->mm);
            if (!p->mm) { list_del(&p->all_node); kfree(p); thread_free(t); return -ENOMEM; }
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

    struct vfork_done vd;
    if (flags & CLONE_VFORK) {
        vd.done = false;
        wait_queue_init(&vd.wq);
        p->vfork = &vd;
    }
    int tid = t->tid;
    thread_start(t);
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

static void release_thread_tid(struct thread *t) {
    if (t->clear_child_tid && t->proc && t->proc->mm) {
        int zero = 0;
        if (!copy_to_user(t->clear_child_tid, &zero, sizeof zero)) futex_wake((uint32_t *)t->clear_child_tid, 1);
    }
}

__noreturn void thread_exit_only(void) {
    fd_borrow_release();
    release_thread_tid(current);
    arch_irq_disable();
    list_del(&current->proc_node);
    thread_exit();
}

__noreturn void process_exit(int status) {
    struct process *p = curproc;
    if (p == init_proc) panic("init exited with status %x", status);
    fd_borrow_release();
    /* other threads of this process are killed on their next return to user mode */
    list_for_each(it, &p->threads) {
        struct thread *t = list_entry(it, struct thread, proc_node);
        if (t != current) { t->killed = true; thread_wake(t); }
    }
    release_thread_tid(current);
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
    if (p->vfork) { p->vfork->done = true; wake_up(&p->vfork->wq); p->vfork = nullptr; }
    struct process *parent = p->parent;
    if (parent) {
        signal_send_internal_chld(parent, p);
        wake_up(&parent->child_wait);
    }
    thread_exit();
}

static bool child_matches(struct process *c, int pid, struct process *self) {
    if (pid > 0) return c->pid == pid;
    if (pid == -1) return true;
    if (pid == 0) return c->pgid == self->pgid;
    return c->pgid == -pid;
}

#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 8

int64_t do_wait(int pid, int *ustatus, int options, int *out_pid) {
    struct process *self = curproc;
    uint64_t irqf = arch_irq_save();
    int64_t ret;
    for (;;) {
        bool any = false;
        list_for_each_safe(it, tmp, &self->children) {
            struct process *c = list_entry(it, struct process, sibling);
            if (!child_matches(c, pid, self)) continue;
            any = true;
            int status = -1;
            if (c->state == P_ZOMBIE) {
                status = c->exit_status;
                int cpid = c->pid;
                struct rusage_k cru = { c->utime_ns + c->cutime_ns, c->stime_ns + c->cstime_ns,
                                        c->min_flt + c->cmin_flt, c->nvcsw + c->cnvcsw, c->nivcsw + c->cnivcsw };
                self->cutime_ns += cru.utime_ns; self->cstime_ns += cru.stime_ns;
                self->cmin_flt += cru.min_flt; self->cnvcsw += cru.nvcsw; self->cnivcsw += cru.nivcsw;
                current->reaped_ru = cru;
                list_del(&c->sibling);
                list_del(&c->all_node);
                kfree(c->cmdline); kfree(c->exe);
                kfree(c);
                ret = cpid;
                if (ustatus && copy_to_user(ustatus, &status, sizeof status)) ret = -EFAULT;
                goto out;
            }
            if ((options & WUNTRACED) && c->stopped && !c->stop_reported) {
                c->stop_reported = true;
                status = (c->stop_sig << 8) | 0x7f;
            } else if ((options & WCONTINUED) && c->cont_reported) {
                c->cont_reported = false;
                status = 0xffff;
            }
            if (status != -1) {
                ret = c->pid;
                if (ustatus && copy_to_user(ustatus, &status, sizeof status)) ret = -EFAULT;
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
