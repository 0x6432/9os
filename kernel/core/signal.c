/* POSIX signals: generation, delivery, default actions, job control stops. */
#include <kernel/signal.h>
#include <kernel/process.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/mm.h>
#include <arch/syscall.h>
#include <arch/unistd.h>
#include <kernel/time.h>
#include <kernel/vfs.h>

static bool no_restart(uint64_t nr) {
    return nr == __NR_nanosleep || nr == __NR_clock_nanosleep || nr == __NR_ppoll || nr == __NR_pselect6 ||
           nr == __NR_rt_sigsuspend
#ifdef __NR_poll
           || nr == __NR_poll || nr == __NR_select || nr == __NR_pause
#endif
           ;
}

int arch_setup_signal_frame(struct trap_frame *f, int sig, struct k_sigaction *ka, uint64_t oldmask);
int arch_sigreturn(struct trap_frame *f, uint64_t *mask);

#define UNBLOCKABLE (SIGBIT(SIGKILL) | SIGBIT(SIGSTOP))
#define STOP_SIGS (SIGBIT(SIGSTOP) | SIGBIT(SIGTSTP) | SIGBIT(SIGTTIN) | SIGBIT(SIGTTOU))
#define IGNORE_SIGS (SIGBIT(SIGCHLD) | SIGBIT(SIGURG) | SIGBIT(SIGWINCH) | SIGBIT(SIGCONT))
#define CORE_SIGS (SIGBIT(SIGQUIT) | SIGBIT(SIGILL) | SIGBIT(SIGTRAP) | SIGBIT(SIGABRT) | \
                   SIGBIT(SIGBUS) | SIGBIT(SIGFPE) | SIGBIT(SIGSEGV) | SIGBIT(SIGSYS))

static struct wait_queue stop_wq = WAIT_QUEUE_INIT(stop_wq);

static uint64_t deliverable(struct thread *t) {
    if (!t->proc) return 0;
    return (t->sig_pending | t->proc->sig_pending) & (~t->sig_mask | UNBLOCKABLE);
}

bool signal_pending(struct thread *t) { return t->killed || deliverable(t) != 0; }

static void kick_thread(struct thread *t) { thread_interrupt(t); }

static void continue_process(struct process *p) {
    if (!p->stopped) return;
    p->stopped = false;
    p->cont_reported = true;
    wake_up(&stop_wq);
    if (p->parent) wake_up(&p->parent->child_wait);
}

void signal_send(struct process *p, int sig) {
    if (!p || sig <= 0 || sig >= NSIG || p->state == P_ZOMBIE) return;
    uint64_t f = arch_irq_save();
    uint64_t bit = SIGBIT(sig);
    struct k_sigaction *ka = &p->sigactions[sig];
    if (sig == SIGKILL || sig == SIGCONT) {
        p->sig_pending &= ~STOP_SIGS;
        continue_process(p);
    }
    if (bit & STOP_SIGS) p->sig_pending &= ~SIGBIT(SIGCONT);
    /* like Linux, a signal blocked by the (first) thread is queued even if its default action
     * is to ignore it, so sigwait/signalfd can still collect e.g. SIGCHLD */
    struct thread *t0 = list_empty(&p->threads) ? nullptr : list_entry(p->threads.next, struct thread, proc_node);
    bool blocked = t0 && (t0->sig_mask & bit);
    bool ignored = sig != SIGKILL && sig != SIGSTOP &&
                   (ka->handler == SIG_IGN || (ka->handler == SIG_DFL && (bit & IGNORE_SIGS) && !blocked));
    if (!ignored) {
        p->sig_pending |= bit;
        list_for_each(it, &p->threads) {
            struct thread *t = list_entry(it, struct thread, proc_node);
            if (!(t->sig_mask & bit) || (bit & UNBLOCKABLE)) { kick_thread(t); break; }
        }
        poll_notify();                       /* signalfd readers */
    }
    arch_irq_restore(f);
}

struct pg_ctx { int pgid, sig; };
static void pg_fn(struct process *p, void *c) {
    struct pg_ctx *x = c;
    if (p->pgid == x->pgid && p->state == P_ALIVE) signal_send(p, x->sig);
}
void signal_send_pgrp(int pgid, int sig) {
    struct pg_ctx c = { pgid, sig };
    process_list(pg_fn, &c);
}

void signal_force(struct thread *t, int sig) {
    struct process *p = t->proc;
    /* a synchronous fault that is blocked or ignored kills the process */
    if ((t->sig_mask & SIGBIT(sig)) || p->sigactions[sig].handler == SIG_IGN) {
        t->sig_mask &= ~SIGBIT(sig);
        p->sigactions[sig].handler = SIG_DFL;
    }
    t->sig_pending |= SIGBIT(sig);
}

void signal_send_internal_chld(struct process *parent, struct process *child) {
    struct k_sigaction *ka = &parent->sigactions[SIGCHLD];
    if (ka->handler != SIG_IGN) signal_send(parent, SIGCHLD);   /* SIG_DFL: queued only if blocked (signalfd) */
}

static void do_stop(struct process *p, int sig) {
    p->stopped = true;
    p->stop_reported = false;
    p->stop_sig = sig;
    if (p->parent) {
        if (!(p->parent->sigactions[SIGCHLD].flags & SA_NOCLDSTOP)) signal_send_internal_chld(p->parent, p);
        wake_up(&p->parent->child_wait);
    }
    while (p->stopped && !current->killed && !(p->sig_pending & SIGBIT(SIGKILL)))
        wait_event(&stop_wq);
}

/* Called on every return to user mode with interrupts disabled. */
void signal_deliver(struct trap_frame *f) {
    struct thread *t = current;
    struct process *p = t->proc;
    if (t->killed) thread_exit_only();
    uint64_t d = deliverable(t);
    if (!d) return;
    int sig = __builtin_ctzll(d) + 1;
    uint64_t bit = SIGBIT(sig);
    if (t->sig_pending & bit) t->sig_pending &= ~bit; else p->sig_pending &= ~bit;
    struct k_sigaction *ka = &p->sigactions[sig];

    /* syscall restart handling */
    bool in_syscall = FRAME_IS_SYSCALL(f);
    int64_t ret = (int64_t)SC_RET(f);
    if (in_syscall && ret == -EINTR && ka->handler != SIG_DFL && ka->handler != SIG_IGN &&
        (ka->flags & SA_RESTART)) {
        uint64_t nr = t->last_syscall;
        if (!no_restart(nr)) frame_restart_syscall(f, nr);
    }

    if (ka->handler == SIG_IGN) return;
    if (ka->handler == SIG_DFL) {
        if (bit & IGNORE_SIGS) return;
        if (bit & STOP_SIGS) { arch_irq_enable(); do_stop(p, sig); arch_irq_disable(); return; }
        arch_irq_enable();
        process_exit(sig | ((bit & CORE_SIGS) ? 0x80 : 0));
    }
    uint64_t oldmask = t->restore_mask ? t->saved_mask : t->sig_mask;
    t->restore_mask = false;
    if (arch_setup_signal_frame(f, sig, ka, oldmask)) {
        arch_irq_enable();
        process_exit(SIGSEGV | 0x80);
    }
    t->sig_mask |= ka->mask;
    if (!(ka->flags & SA_NODEFER)) t->sig_mask |= bit;
    t->sig_mask &= ~UNBLOCKABLE;
    if (ka->flags & SA_RESETHAND) ka->handler = SIG_DFL;
}

void signals_reset_on_exec(struct process *p) {
    for (int i = 1; i < NSIG; i++) {
        if (p->sigactions[i].handler != SIG_IGN) p->sigactions[i].handler = SIG_DFL;
        p->sigactions[i].flags = 0;
        p->sigactions[i].mask = 0;
    }
    current->altstack_sp = current->altstack_size = 0;
}

/* ---- syscalls ---- */

int64_t sys_rt_sigaction(int sig, const struct k_sigaction *act, struct k_sigaction *old, size_t sz) {
    if (sig <= 0 || sig >= NSIG) return -EINVAL;
    struct process *p = curproc;
    struct k_sigaction ka;
    if (act && copy_from_user(&ka, act, sizeof ka)) return -EFAULT;
    if (old && copy_to_user(old, &p->sigactions[sig], sizeof *old)) return -EFAULT;
    if (act) {
        if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;
        ka.mask &= ~UNBLOCKABLE;
        p->sigactions[sig] = ka;
        if (ka.handler == SIG_IGN || (ka.handler == SIG_DFL && (SIGBIT(sig) & IGNORE_SIGS))) {
            p->sig_pending &= ~SIGBIT(sig);
            current->sig_pending &= ~SIGBIT(sig);
        }
    }
    return 0;
}

int64_t sys_rt_sigprocmask(int how, const uint64_t *set, uint64_t *old, size_t sz) {
    struct thread *t = current;
    uint64_t n;
    if (old && copy_to_user(old, &t->sig_mask, 8)) return -EFAULT;
    if (!set) return 0;
    if (copy_from_user(&n, set, 8)) return -EFAULT;
    switch (how) {
    case SIG_BLOCK: t->sig_mask |= n; break;
    case SIG_UNBLOCK: t->sig_mask &= ~n; break;
    case SIG_SETMASK: t->sig_mask = n; break;
    default: return -EINVAL;
    }
    t->sig_mask &= ~UNBLOCKABLE;
    return 0;
}

int64_t sys_rt_sigpending(uint64_t *set, size_t sz) {
    uint64_t v = (current->sig_pending | curproc->sig_pending) & current->sig_mask;
    return copy_to_user(set, &v, 8);
}

int64_t sys_rt_sigsuspend(const uint64_t *set, size_t sz) {
    uint64_t n;
    if (copy_from_user(&n, set, 8)) return -EFAULT;
    struct thread *t = current;
    t->saved_mask = t->sig_mask;
    t->restore_mask = true;
    t->sig_mask = n & ~UNBLOCKABLE;
    static struct wait_queue never = WAIT_QUEUE_INIT(never);
    while (!signal_pending(t)) wait_event(&never);
    return -EINTR;
}

int64_t sys_pause(void) {
    static struct wait_queue never = WAIT_QUEUE_INIT(never);
    while (!signal_pending(current)) wait_event(&never);
    return -EINTR;
}

struct trap_frame *thread_user_frame(struct thread *t);

int64_t sys_rt_sigreturn(void) {
    struct trap_frame *f = thread_user_frame(current);
    uint64_t mask;
    if (arch_sigreturn(f, &mask)) process_exit(SIGSEGV);
    current->sig_mask = mask & ~UNBLOCKABLE;
    return (int64_t)SC_RET(f);      /* keep restored rax / a0 / x0 */
}

int64_t sys_sigaltstack(const uint64_t *ss, uint64_t *old) {
    struct thread *t = current;
    if (old) {
        uint64_t o[3] = { t->altstack_sp, (uint64_t)(t->altstack_size ? 0 : 2), t->altstack_size };
        if (copy_to_user(old, o, 24)) return -EFAULT;
    }
    if (ss) {
        uint64_t n[3];
        if (copy_from_user(n, ss, 24)) return -EFAULT;
        if ((int)n[1] & 2) { t->altstack_sp = t->altstack_size = 0; }
        else { t->altstack_sp = n[0]; t->altstack_size = n[2]; }
    }
    return 0;
}

/* M31: the sender's real or effective uid must match the target's real or saved uid, or
 * CAP_KILL; SIGCONT is always allowed within the session (job control across su). */
static bool kill_perm(struct process *p, int sig) {
    if (p == curproc || capable(CAP_KILL)) return true;
    if (sig == SIGCONT && p->sid == curproc->sid) return true;
    const struct cred *c = current_cred();
    struct cred *t = proc_cred(p);
    bool ok = c->euid == t->suid || c->euid == t->uid || c->uid == t->suid || c->uid == t->uid;
    cred_put(t);
    return ok;
}

static int kill_one(struct process *p, int sig) {
    if (!p || p->state == P_ZOMBIE) return -ESRCH;
    if (!kill_perm(p, sig)) return -EPERM;
    if (sig) signal_send(p, sig);
    return 0;
}

struct kill_ctx { int pgid, sig, count, denied; struct process *self; bool all; };
static void kill_fn(struct process *p, void *c) {
    struct kill_ctx *k = c;
    if (p->state != P_ALIVE || p->pid == 1) return;
    if (k->all ? p != k->self : p->pgid == k->pgid) {
        if (!kill_perm(p, k->sig)) { k->denied++; return; }
        if (k->sig) signal_send(p, k->sig);
        k->count++;
    }
}

int64_t sys_kill(int pid, int sig) {
    if (sig < 0 || sig >= NSIG) return -EINVAL;
    if (pid > 0) return kill_one(process_find(pid), sig);
    struct kill_ctx k = { pid == 0 ? curproc->pgid : -pid, sig, 0, 0, curproc, pid == -1 };
    process_list(kill_fn, &k);
    return k.count ? 0 : k.denied ? -EPERM : -ESRCH;
}

int64_t sys_tgkill(int tgid, int tid, int sig) {
    struct process *p = process_find(tgid);
    if (!p) {
        /* tid may be a non-main thread; fall back to process lookup by tid */
        return -ESRCH;
    }
    if (!kill_perm(p, sig)) return -EPERM;
    if (sig) {
        list_for_each(it, &p->threads) {
            struct thread *t = list_entry(it, struct thread, proc_node);
            if (t->tid == tid) {
                t->sig_pending |= SIGBIT(sig);
                /* a blocked signal stays pending without interrupting the thread's sleep */
                if (!(t->sig_mask & SIGBIT(sig)) || (SIGBIT(sig) & UNBLOCKABLE)) kick_thread(t);
                poll_notify();               /* signalfd readers */
                return 0;
            }
        }
        signal_send(p, sig);
    }
    return 0;
}
int64_t sys_tkill(int tid, int sig) { return sys_tgkill(tid, tid, sig); }

int64_t sys_rt_sigtimedwait(const uint64_t *uset, void *uinfo, const struct timespec *uts, size_t sz) {
    uint64_t set;
    if (copy_from_user(&set, uset, 8)) return -EFAULT;
    uint64_t ns = UINT64_MAX;
    if (uts) {
        struct timespec ts;
        if (copy_from_user(&ts, uts, sizeof ts)) return -EFAULT;
        ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    }
    struct thread *t = current;
    struct process *p = t->proc;
    static struct wait_queue never = WAIT_QUEUE_INIT(never);
    /* temporarily unblock the waited-for signals so that senders wake us */
    uint64_t oldmask = t->sig_mask;
    uint64_t deadline = ns == UINT64_MAX ? UINT64_MAX : time_ns() + ns;
    for (;;) {
        uint64_t f = arch_irq_save();
        uint64_t pend = (t->sig_pending | p->sig_pending) & set;
        if (pend) {
            int sig = __builtin_ctzll(pend) + 1;
            uint64_t bit = SIGBIT(sig);
            if (t->sig_pending & bit) t->sig_pending &= ~bit; else p->sig_pending &= ~bit;
            arch_irq_restore(f);
            t->sig_mask = oldmask;
            if (uinfo) {
                int32_t si[32] = {0};
                si[0] = sig;
                copy_to_user(uinfo, si, sizeof si);
            }
            return sig;
        }
        uint64_t now = time_ns();
        if (now >= deadline) { arch_irq_restore(f); t->sig_mask = oldmask; return -EAGAIN; }
        t->sig_mask = oldmask & ~set;
        int r = wait_event_timeout(&never, deadline == UINT64_MAX ? UINT64_MAX : deadline - now);
        t->sig_mask = oldmask;
        arch_irq_restore(f);
        if (r == -EINTR) {
            /* woken by a signal: loop to see if it is one we wait for */
            if (!((t->sig_pending | p->sig_pending) & set) && signal_pending(t)) return -EINTR;
        }
    }
}
