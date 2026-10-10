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
#include <kernel/kmalloc.h>

static bool no_restart(uint64_t nr) {
    return nr == __NR_nanosleep || nr == __NR_clock_nanosleep || nr == __NR_ppoll || nr == __NR_pselect6 ||
           nr == __NR_rt_sigsuspend || nr == __NR_restart_syscall
#ifdef __NR_poll
           || nr == __NR_poll || nr == __NR_select || nr == __NR_pause
#endif
           ;
}

int arch_setup_signal_frame(struct trap_frame *f, int sig, struct k_sigaction *ka, uint64_t oldmask,
                            const struct ksiginfo *ki);
int arch_sigreturn(struct trap_frame *f, uint64_t *mask);

#define UNBLOCKABLE (SIGBIT(SIGKILL) | SIGBIT(SIGSTOP))
#define STOP_SIGS (SIGBIT(SIGSTOP) | SIGBIT(SIGTSTP) | SIGBIT(SIGTTIN) | SIGBIT(SIGTTOU))
#define IGNORE_SIGS (SIGBIT(SIGCHLD) | SIGBIT(SIGURG) | SIGBIT(SIGWINCH) | SIGBIT(SIGCONT))
#define CORE_SIGS (SIGBIT(SIGQUIT) | SIGBIT(SIGILL) | SIGBIT(SIGTRAP) | SIGBIT(SIGABRT) | \
                   SIGBIT(SIGBUS) | SIGBIT(SIGFPE) | SIGBIT(SIGSEGV) | SIGBIT(SIGSYS))

#define SYNC_SIGS (SIGBIT(SIGSEGV) | SIGBIT(SIGBUS) | SIGBIT(SIGILL) | SIGBIT(SIGTRAP) | SIGBIT(SIGFPE) | SIGBIT(SIGSYS))
#define SIGRTMIN_ 32

static struct wait_queue stop_wq = WAIT_QUEUE_INIT(stop_wq);

int ptrace_signal_stop(struct trap_frame *f, struct ksiginfo *ki);    /* ptrace.c */
void posix_timer_dequeued(struct process *p, struct ksiginfo *ki);
bool ptrace_group_stop(int sig);
void ptrace_kill_wake(struct process *p);
void ptrace_cont_notify(struct process *p);
void ptrace_interrupt_stop(void);

/* ---- siginfo queues (M33) ----
 * sig_pending stays the authoritative bitmask. Standard signals keep one record each (a
 * second instance while pending is merged), real-time signals queue every instance. All
 * updates happen with interrupts off (signals are also sent from IRQ handlers). */
static void ksi_init(struct ksiginfo *ki, int sig, int code) {
    memset(ki, 0, sizeof *ki);
    ki->signo = sig; ki->code = code;
}
void ksiginfo_user(struct ksiginfo *ki, int sig, int code) {
    ksi_init(ki, sig, code);
    if (current && current->proc) { ki->pid = current->proc->pid; ki->uid = current_cred()->uid; }
}

/* *np: preallocated node for real-time signals, cleared when consumed. False if merged. */
static bool sigq_add(uint64_t *bits, struct sigpend *q, const struct ksiginfo *ki, struct sigq_node **np) {
    int sig = ki->signo;
    uint64_t bit = SIGBIT(sig);
    if (sig < SIGRTMIN_) {
        if (*bits & bit) return false;
        q->std[sig] = *ki;
    } else if (*np) {
        if (!q->rtq.next) list_init(&q->rtq);
        (*np)->info = *ki;
        list_add_tail(&q->rtq, &(*np)->node);
        *np = nullptr;
    }
    *bits |= bit;
    return true;
}

static void sigq_take(uint64_t *bits, struct sigpend *q, int sig, struct ksiginfo *out) {
    uint64_t bit = SIGBIT(sig);
    if (sig < SIGRTMIN_) {
        *out = q->std[sig];
        if (out->signo != sig) ksi_init(out, sig, SI_KERNEL);
        q->std[sig].signo = 0;
        *bits &= ~bit;
        return;
    }
    struct sigq_node *found = nullptr;
    bool more = false;
    if (q->rtq.next) list_for_each(it, &q->rtq) {
        struct sigq_node *n = list_entry(it, struct sigq_node, node);
        if (n->info.signo != sig) continue;
        if (found) { more = true; break; }
        found = n;
    }
    if (found) { *out = found->info; list_del(&found->node); kfree(found); }
    else ksi_init(out, sig, SI_USER);           /* queue overflow: the instance lost its info */
    if (!more) *bits &= ~bit;
}

static void sigq_discard(uint64_t *bits, struct sigpend *q, uint64_t mask) {
    *bits &= ~mask;
    if (!q->rtq.next) return;
    list_for_each_safe(it, tmp, &q->rtq) {
        struct sigq_node *n = list_entry(it, struct sigq_node, node);
        if (mask & SIGBIT(n->info.signo)) { list_del(&n->node); kfree(n); }
    }
}
void sigq_flush_thread(struct thread *t) { sigq_discard(&t->sig_pending, &t->sigq, ~0ULL); }
void sigq_flush_proc(struct process *p) { sigq_discard(&p->sig_pending, &p->sigq, ~0ULL); }

/* pick the next deliverable signal in mask: synchronous faults first, then the lowest */
int signal_dequeue(struct thread *t, uint64_t mask, struct ksiginfo *out) {
    struct process *p = t->proc;
    uint64_t pend = (t->sig_pending | (p ? p->sig_pending : 0)) & mask;
    if (!pend) return 0;
    uint64_t sync = pend & SYNC_SIGS;
    int sig = __builtin_ctzll(sync ? sync : pend) + 1;
    if (t->sig_pending & SIGBIT(sig)) sigq_take(&t->sig_pending, &t->sigq, sig, out);
    else sigq_take(&p->sig_pending, &p->sigq, sig, out);
    if (out->code == SI_TIMER) posix_timer_dequeued(p, out);
    return sig;
}

/* Linux siginfo_t (128 bytes): signo, errno, code, then the union at offset 16 */
void siginfo_to_user(const struct ksiginfo *ki, void *out) {
    uint8_t *b = out;
    int32_t *w = out;
    memset(b, 0, 128);
    w[0] = ki->signo; w[1] = ki->err; w[2] = ki->code;
    int s = ki->signo, c = ki->code;
    bool kern = c > 0 && c < SI_KERNEL;
    if (c == SI_TIMER) { w[4] = ki->i2; w[5] = ki->i1; memcpy(b + 24, &ki->v, 8); }
    else if (s == SIGCHLD && kern) { w[4] = ki->pid; w[5] = (int32_t)ki->uid; w[6] = ki->i1; memcpy(b + 32, &ki->v, 8); memcpy(b + 40, &ki->v2, 8); }
    else if (kern && (SIGBIT(s) & (SIGBIT(SIGSEGV) | SIGBIT(SIGBUS) | SIGBIT(SIGILL) | SIGBIT(SIGFPE) | SIGBIT(SIGTRAP)))) memcpy(b + 16, &ki->v, 8);
    else if (kern && s == SIGSYS) { memcpy(b + 16, &ki->v, 8); w[6] = ki->i1; w[7] = ki->i2; }
    else if (kern && s == SIGIO) { memcpy(b + 16, &ki->v, 8); w[6] = ki->i1; }
    else { w[4] = ki->pid; w[5] = (int32_t)ki->uid; memcpy(b + 24, &ki->v, 8); }
}
void siginfo_from_user(struct ksiginfo *ki, const void *in) {
    const uint8_t *b = in;
    const int32_t *w = in;
    memset(ki, 0, sizeof *ki);
    ki->signo = w[0]; ki->err = w[1]; ki->code = w[2];
    int s = ki->signo, c = ki->code;
    bool kern = c > 0 && c < SI_KERNEL;
    if (c == SI_TIMER) { ki->i2 = w[4]; ki->i1 = w[5]; memcpy(&ki->v, b + 24, 8); }
    else if (s == SIGCHLD && kern) { ki->pid = w[4]; ki->uid = (uint32_t)w[5]; ki->i1 = w[6]; memcpy(&ki->v, b + 32, 8); memcpy(&ki->v2, b + 40, 8); }
    else if (kern && (SIGBIT(s) & (SIGBIT(SIGSEGV) | SIGBIT(SIGBUS) | SIGBIT(SIGILL) | SIGBIT(SIGFPE) | SIGBIT(SIGTRAP)))) memcpy(&ki->v, b + 16, 8);
    else if (kern && s == SIGSYS) { memcpy(&ki->v, b + 16, 8); ki->i1 = w[6]; ki->i2 = w[7]; }
    else if (kern && s == SIGIO) { memcpy(&ki->v, b + 16, 8); ki->i1 = w[6]; }
    else { ki->pid = w[4]; ki->uid = (uint32_t)w[5]; memcpy(&ki->v, b + 24, 8); }
}

static uint64_t deliverable(struct thread *t) {
    if (!t->proc) return 0;
    return (t->sig_pending | t->proc->sig_pending) & (~t->sig_mask | UNBLOCKABLE);
}

bool signal_pending(struct thread *t) { return t->killed || t->pt_interrupt || deliverable(t) != 0; }

static void kick_thread(struct thread *t) { thread_interrupt(t); }

static void continue_process(struct process *p) {
    if (!p->stopped) return;
    p->stopped = false;
    p->cont_reported = true;
    wake_up(&stop_wq);
    if (p->parent) {
        if (!(p->parent->sigactions[SIGCHLD].flags & SA_NOCLDSTOP)) signal_send_chld(p->parent, p, CLD_CONTINUED, SIGCONT);
        wake_up(&p->parent->child_wait);
    }
}

void signal_send_info(struct process *p, const struct ksiginfo *ki) {
    int sig = ki->signo;
    if (!p || sig <= 0 || sig >= NSIG || p->state == P_ZOMBIE) return;
    struct sigq_node *n = sig >= SIGRTMIN_ ? kmalloc(sizeof *n) : nullptr;
    uint64_t f = arch_irq_save();
    uint64_t bit = SIGBIT(sig);
    struct k_sigaction *ka = &p->sigactions[sig];
    if (sig == SIGKILL || sig == SIGCONT) {
        sigq_discard(&p->sig_pending, &p->sigq, STOP_SIGS);
        continue_process(p);
        if (sig == SIGKILL) ptrace_kill_wake(p);
        else ptrace_cont_notify(p);
    }
    if (bit & STOP_SIGS) p->sig_pending &= ~SIGBIT(SIGCONT);
    /* like Linux, a signal blocked by the (first) thread is queued even if its default action
     * is to ignore it, so sigwait/signalfd can still collect e.g. SIGCHLD */
    struct thread *t0 = list_empty(&p->threads) ? nullptr : list_entry(p->threads.next, struct thread, proc_node);
    bool blocked = t0 && (t0->sig_mask & bit);
    bool traced = t0 && t0->ptracer;       /* the tracer sees every signal, even ignored ones */
    bool ignored = sig != SIGKILL && sig != SIGSTOP && !traced &&
                   (ka->handler == SIG_IGN || (ka->handler == SIG_DFL && (bit & IGNORE_SIGS) && !blocked));
    if (!ignored) {
        sigq_add(&p->sig_pending, &p->sigq, ki, &n);
        list_for_each(it, &p->threads) {
            struct thread *t = list_entry(it, struct thread, proc_node);
            if (!(t->sig_mask & bit) || (bit & UNBLOCKABLE)) { kick_thread(t); break; }
        }
        poll_notify();                       /* signalfd readers */
    }
    arch_irq_restore(f);
    if (n) kfree(n);
}

void signal_send(struct process *p, int sig) {
    struct ksiginfo ki;
    ksi_init(&ki, sig, SI_KERNEL);
    signal_send_info(p, &ki);
}

/* thread-directed signal (tgkill, SIGEV_THREAD_ID timers, ptrace). Stop/continue/kill
 * signals act on the whole process. */
int signal_thread_info(struct thread *t, const struct ksiginfo *ki) {
    int sig = ki->signo;
    struct process *p = t->proc;
    if (!p || p->state == P_ZOMBIE) return -ESRCH;
    uint64_t bit = SIGBIT(sig);
    if (sig == SIGKILL || sig == SIGCONT || (bit & STOP_SIGS)) { signal_send_info(p, ki); return 0; }
    struct k_sigaction *ka = &p->sigactions[sig];
    if (!t->ptracer && (ka->handler == SIG_IGN || (ka->handler == SIG_DFL && (bit & IGNORE_SIGS) && !(t->sig_mask & bit))))
        return 0;
    struct sigq_node *n = sig >= SIGRTMIN_ ? kmalloc(sizeof *n) : nullptr;
    if (sig >= SIGRTMIN_ && !n) return -EAGAIN;
    uint64_t f = arch_irq_save();
    sigq_add(&t->sig_pending, &t->sigq, ki, &n);
    /* a blocked signal stays pending without interrupting the thread's sleep */
    if (!(t->sig_mask & bit)) kick_thread(t);
    poll_notify();                           /* signalfd readers */
    arch_irq_restore(f);
    if (n) kfree(n);
    return 0;
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

void signal_force_info(struct thread *t, int sig, int code, uint64_t addr) {
    struct process *p = t->proc;
    /* a synchronous fault that is blocked or ignored kills the process */
    if ((t->sig_mask & SIGBIT(sig)) || p->sigactions[sig].handler == SIG_IGN) {
        t->sig_mask &= ~SIGBIT(sig);
        p->sigactions[sig].handler = SIG_DFL;
    }
    uint64_t f = arch_irq_save();
    ksi_init(&t->sigq.std[sig], sig, code);
    t->sigq.std[sig].v = addr;
    t->sig_pending |= SIGBIT(sig);
    arch_irq_restore(f);
}
/* queue sig on thread t regardless of its disposition (ptrace: attach SIGSTOP, post-exec SIGTRAP);
 * kick = false for a thread that is not running yet (fork) or is current */
void signal_thread_queue(struct thread *t, int sig, int code, bool kick) {
    struct ksiginfo ki;
    ksiginfo_user(&ki, sig, code);
    uint64_t f = arch_irq_save();
    struct sigq_node *n = nullptr;
    sigq_add(&t->sig_pending, &t->sigq, &ki, &n);
    if (kick) kick_thread(t);
    arch_irq_restore(f);
}
void signal_force(struct thread *t, int sig) { signal_force_info(t, sig, SI_KERNEL, 0); }

/* SIGCHLD (or the clone3 exit signal) to the parent, with the child's wait status */
void signal_send_chld(struct process *parent, struct process *child, int code, int status) {
    int sig = code == CLD_EXITED || code == CLD_KILLED || code == CLD_DUMPED ? child->exit_signal : SIGCHLD;
    if (!sig || !parent) return;
    if (sig == SIGCHLD && parent->sigactions[SIGCHLD].handler == SIG_IGN) return;  /* SIG_DFL: queued only if blocked (signalfd) */
    struct ksiginfo ki;
    ksi_init(&ki, sig, code);
    ki.pid = child->pid;
    ki.uid = child->cred ? child->cred->uid : 0;
    ki.i1 = status;
    ki.v = child->utime_ns / 10000000;         /* clock_t, USER_HZ = 100 */
    ki.v2 = child->stime_ns / 10000000;
    signal_send_info(parent, &ki);
}
void signal_send_internal_chld(struct process *parent, struct process *child) {
    signal_send_chld(parent, child, CLD_STOPPED, child->stop_sig);
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

/* the syscall interrupted by a signal that runs no handler is restarted transparently */
static void maybe_restart(struct trap_frame *f, struct thread *t) {
    if (!FRAME_IS_SYSCALL(f) || (int64_t)SC_RET(f) != -EINTR) return;
    uint64_t nr = t->last_syscall;
    if (!no_restart(nr)) frame_restart_syscall(f, nr);
    else if ((nr == __NR_nanosleep || nr == __NR_clock_nanosleep || nr == __NR_restart_syscall) && t->restart_sleep)
        frame_restart_syscall(f, __NR_restart_syscall);     /* resume with the remaining time */
    else if (nr == __NR_rt_sigsuspend
#ifdef __NR_pause
             || nr == __NR_pause
#endif
            ) frame_restart_syscall(f, nr);                 /* ERESTARTNOHAND: same arguments */
}

/* Called on every return to user mode with interrupts disabled. */
void signal_deliver(struct trap_frame *f) {
    struct thread *t = current;
    struct process *p = t->proc;
    if (t->killed) thread_exit_only();
    if (t->pt_interrupt) {                          /* PTRACE_INTERRUPT: PTRACE_EVENT_STOP */
        arch_irq_enable();
        ptrace_interrupt_stop();
        arch_irq_disable();
        if (t->killed) thread_exit_only();
        maybe_restart(f, t);
        return;
    }
    if (!deliverable(t)) return;
    struct ksiginfo ki;
    int sig = signal_dequeue(t, ~t->sig_mask | UNBLOCKABLE, &ki);
    if (!sig) return;
    if (t->ptracer && sig != SIGKILL) {
        /* signal-delivery-stop: the tracer may suppress, keep or replace the signal */
        arch_irq_enable();
        sig = ptrace_signal_stop(f, &ki);
        arch_irq_disable();
        if (t->killed) thread_exit_only();
        if (!sig) { maybe_restart(f, t); return; }
        if (t->sig_mask & SIGBIT(sig)) {            /* blocked: requeue for later */
            struct sigq_node *n = nullptr;
            if (sig >= SIGRTMIN_) {
                arch_irq_enable(); n = kmalloc(sizeof *n); arch_irq_disable();
            }
            sigq_add(&t->sig_pending, &t->sigq, &ki, &n);
            if (n) kfree(n);
            return;
        }
    }
    uint64_t bit = SIGBIT(sig);
    struct k_sigaction *ka = &p->sigactions[sig];

    /* syscall restart handling */
    bool in_syscall = FRAME_IS_SYSCALL(f);
    int64_t ret = (int64_t)SC_RET(f);
    if (in_syscall && ret == -EINTR && ka->handler != SIG_DFL && ka->handler != SIG_IGN &&
        (ka->flags & SA_RESTART)) {
        uint64_t nr = t->last_syscall;
        if (!no_restart(nr)) frame_restart_syscall(f, nr);
    }

    if (ka->handler == SIG_IGN) { maybe_restart(f, t); return; }
    if (ka->handler == SIG_DFL) {
        if (bit & IGNORE_SIGS) { maybe_restart(f, t); return; }
        if (bit & STOP_SIGS) {
            arch_irq_enable();
            if (!(t->ptracer && ptrace_group_stop(sig))) do_stop(p, sig);
            arch_irq_disable();
            maybe_restart(f, t);
            return;
        }
        arch_irq_enable();
        process_exit(sig | ((bit & CORE_SIGS) ? 0x80 : 0));
    }
    uint64_t oldmask = t->restore_mask ? t->saved_mask : t->sig_mask;
    t->restore_mask = false;
    if (arch_setup_signal_frame(f, sig, ka, oldmask, &ki)) {
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
            uint64_t f = arch_irq_save();
            sigq_discard(&p->sig_pending, &p->sigq, SIGBIT(sig));
            list_for_each(it, &p->threads)
                sigq_discard(&list_entry(it, struct thread, proc_node)->sig_pending,
                             &list_entry(it, struct thread, proc_node)->sigq, SIGBIT(sig));
            arch_irq_restore(f);
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
    if (sig) { struct ksiginfo ki; ksiginfo_user(&ki, sig, SI_USER); signal_send_info(p, &ki); }
    return 0;
}

struct kill_ctx { int pgid, sig, count, denied; struct process *self; bool all; };
static void kill_fn(struct process *p, void *c) {
    struct kill_ctx *k = c;
    if (p->state != P_ALIVE || p->pid == 1) return;
    if (k->all ? p != k->self : p->pgid == k->pgid) {
        if (!kill_perm(p, k->sig)) { k->denied++; return; }
        if (k->sig) { struct ksiginfo ki; ksiginfo_user(&ki, k->sig, SI_USER); signal_send_info(p, &ki); }
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

static int tgkill_info(int tgid, int tid, const struct ksiginfo *ki, bool perm_self_only) {
    struct thread *t = process_find_thread(tid);
    if (!t || !t->proc || (tgid > 0 && t->proc->pid != tgid) || t->proc->state == P_ZOMBIE) return -ESRCH;
    int sig = ki->signo;
    if (sig < 0 || sig >= NSIG) return -EINVAL;
    if (perm_self_only && t->proc != curproc && (ki->code >= 0 || ki->code == SI_TKILL)) return -EPERM;
    if (!kill_perm(t->proc, sig)) return -EPERM;
    return sig ? signal_thread_info(t, ki) : 0;
}
int64_t sys_tgkill(int tgid, int tid, int sig) {
    if (sig < 0 || sig >= NSIG || tid <= 0 || tgid <= 0) return -EINVAL;
    struct ksiginfo ki;
    ksiginfo_user(&ki, sig, SI_TKILL);
    return tgkill_info(tgid, tid, &ki, false);
}
int64_t sys_tkill(int tid, int sig) {
    if (sig < 0 || sig >= NSIG || tid <= 0) return -EINVAL;
    struct ksiginfo ki;
    ksiginfo_user(&ki, sig, SI_TKILL);
    return tgkill_info(-1, tid, &ki, false);
}

/* sigqueue(3): the caller supplies the siginfo; only si_code < 0 (SI_QUEUE etc.) may be
 * forged for other processes, like Linux */
int64_t sys_rt_sigqueueinfo(int pid, int sig, const void *uinfo) {
    uint8_t raw[128];
    if (copy_from_user(raw, uinfo, 128)) return -EFAULT;
    if (sig <= 0 || sig >= NSIG) return -EINVAL;
    struct ksiginfo ki;
    siginfo_from_user(&ki, raw);
    ki.signo = sig;
    struct process *p = process_find(pid);
    if (!p || p->state == P_ZOMBIE) return -ESRCH;
    if (p != curproc && (ki.code >= 0 || ki.code == SI_TKILL)) return -EPERM;
    if (!kill_perm(p, sig)) return -EPERM;
    signal_send_info(p, &ki);
    return 0;
}
int64_t sys_rt_tgsigqueueinfo(int tgid, int tid, int sig, const void *uinfo) {
    uint8_t raw[128];
    if (copy_from_user(raw, uinfo, 128)) return -EFAULT;
    if (sig <= 0 || sig >= NSIG) return -EINVAL;
    struct ksiginfo ki;
    siginfo_from_user(&ki, raw);
    ki.signo = sig;
    return tgkill_info(tgid, tid, &ki, true);
}

/* pidfd_send_signal(2) helper (pidfd.c) */
int signal_kill_process(struct process *p, int sig, const void *uinfo) {
    if (!p || p->state == P_ZOMBIE) return -ESRCH;
    if (sig < 0 || sig >= NSIG) return -EINVAL;
    struct ksiginfo ki;
    if (uinfo) {
        uint8_t raw[128];
        if (copy_from_user(raw, uinfo, 128)) return -EFAULT;
        siginfo_from_user(&ki, raw);
        if (ki.signo != sig) return -EINVAL;
        if (p != curproc && (ki.code >= 0 || ki.code == SI_TKILL)) return -EPERM;
    } else ksiginfo_user(&ki, sig, SI_USER);
    if (!kill_perm(p, sig)) return -EPERM;
    if (sig) signal_send_info(p, &ki);
    return 0;
}

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
        struct ksiginfo ki;
        int sig = signal_dequeue(t, set, &ki);
        if (sig) {
            arch_irq_restore(f);
            t->sig_mask = oldmask;
            if (uinfo) {
                uint8_t si[128];
                siginfo_to_user(&ki, si);
                if (copy_to_user(uinfo, si, sizeof si)) return -EFAULT;
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
