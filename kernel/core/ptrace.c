/* ptrace(2) (M33): tracer/tracee bookkeeping, ptrace stops, register and memory access.
 *
 * A tracee is a thread (thread.ptracer = tracing process). Every stop is entered by the tracee
 * itself from a safe point (signal delivery, syscall entry/exit, fork/exec/exit events, group
 * stop, PTRACE_INTERRUPT) through pt_stop(): it records the wait status, notifies the tracer
 * (SIGCHLD with CLD_TRAPPED + child_wait) and sleeps on pt_wq until resumed or killed. While it
 * sleeps its user trap frame and saved FPU state are stable, so the tracer reads and writes
 * them directly. All bookkeeping runs under the BKL (ptrace and the slow syscall path take it;
 * a traced thread never uses the lock-free syscall fast path). */
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/syscall.h>
#include <kernel/mm.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/cred.h>
#include <arch/syscall.h>

#define PTRACE_TRACEME 0
#define PTRACE_PEEKTEXT 1
#define PTRACE_PEEKDATA 2
#define PTRACE_PEEKUSER 3
#define PTRACE_POKETEXT 4
#define PTRACE_POKEDATA 5
#define PTRACE_POKEUSER 6
#define PTRACE_CONT 7
#define PTRACE_KILL 8
#define PTRACE_SINGLESTEP 9
#define PTRACE_GETREGS 12
#define PTRACE_SETREGS 13
#define PTRACE_GETFPREGS 14
#define PTRACE_SETFPREGS 15
#define PTRACE_ATTACH 16
#define PTRACE_DETACH 17
#define PTRACE_SYSCALL 24
#define PTRACE_SETOPTIONS 0x4200
#define PTRACE_GETEVENTMSG 0x4201
#define PTRACE_GETSIGINFO 0x4202
#define PTRACE_SETSIGINFO 0x4203
#define PTRACE_GETREGSET 0x4204
#define PTRACE_SETREGSET 0x4205
#define PTRACE_SEIZE 0x4206
#define PTRACE_INTERRUPT 0x4207
#define PTRACE_LISTEN 0x4208
#define PTRACE_GETSIGMASK 0x420a
#define PTRACE_SETSIGMASK 0x420b
#define PTRACE_GET_SYSCALL_INFO 0x420e

#define PTRACE_O_TRACESYSGOOD 1
#define PTRACE_O_TRACEFORK 2
#define PTRACE_O_TRACEVFORK 4
#define PTRACE_O_TRACECLONE 8
#define PTRACE_O_TRACEEXEC 0x10
#define PTRACE_O_TRACEVFORKDONE 0x20
#define PTRACE_O_TRACEEXIT 0x40
#define PTRACE_O_TRACESECCOMP 0x80
#define PTRACE_O_EXITKILL 0x100000
#define PTRACE_O_SUSPEND_SECCOMP 0x200000
#define PTRACE_O_MASK (0xff | PTRACE_O_EXITKILL | PTRACE_O_SUSPEND_SECCOMP)

#define PTRACE_EVENT_FORK 1
#define PTRACE_EVENT_VFORK 2
#define PTRACE_EVENT_CLONE 3
#define PTRACE_EVENT_EXEC 4
#define PTRACE_EVENT_VFORK_DONE 5
#define PTRACE_EVENT_EXIT 6
#define PTRACE_EVENT_STOP 128

#define NT_PRSTATUS 1
#define NT_PRFPREG 2
#define NT_ARM_TLS 0x401
#define NT_ARM_SYSTEM_CALL 0x404

enum { PT_STOP_SIGNAL = 1, PT_STOP_GROUP, PT_STOP_SC_ENTRY, PT_STOP_SC_EXIT, PT_STOP_EVENT, PT_STOP_INTERRUPT };

#define CLONE_VFORK 0x4000
#define WEXITED 4
#define WNOWAIT 0x01000000
#define __WALL 0x40000000
#define __WCLONE 0x80000000
#define UNBLOCKABLE (SIGBIT(SIGKILL) | SIGBIT(SIGSTOP))

struct trap_frame *thread_user_frame(struct thread *t);
void arch_set_tls(struct thread *t, uint64_t v);
void signal_thread_queue(struct thread *t, int sig, int code, bool kick);   /* signal.c */

/* exit of a tracee the normal child reaping does not report (attached non-child, thread) */
struct pt_exit { struct list_node node; int tid, pgid, status; bool clone; };

static struct wait_queue pt_wq = WAIT_QUEUE_INIT(pt_wq);

static bool fatal(struct thread *t) {
    return t->killed || !t->proc || ((t->sig_pending | t->proc->sig_pending) & SIGBIT(SIGKILL));
}

/* ---------------------------------------------------------------- arch: registers, stepping */

#if defined(__x86_64__)
#define ARCH_HAS_STEP 1
#define AUDIT_ARCH 0xc000003eu
#define PRSTATUS_SIZE (27 * 8)
#define FPREGS_SIZE 512
static void arch_step(struct thread *t, bool on) {
    struct trap_frame *f = thread_user_frame(t);
    t->pt_step = on;
    if (on) f->rflags |= 0x100; else f->rflags &= ~0x100ULL;
}
static uint64_t orig_nr(struct thread *t) {
    if (t->pt_why == PT_STOP_SC_ENTRY || t->pt_why == PT_STOP_SC_EXIT) return (uint64_t)(int64_t)t->pt_sc_nr;
    return FRAME_IS_SYSCALL(thread_user_frame(t)) ? t->last_syscall : (uint64_t)-1;
}
/* user_regs_struct: r15 r14 r13 r12 rbp rbx r11 r10 r9 r8 rax rcx rdx rsi rdi orig_rax rip cs
 * eflags rsp ss fs_base gs_base ds es fs gs */
static int regs_get(struct thread *t, uint64_t *r) {
    struct trap_frame *f = thread_user_frame(t);
    uint64_t v[27] = { f->r15, f->r14, f->r13, f->r12, f->rbp, f->rbx, f->r11, f->r10, f->r9, f->r8,
                       f->rax, f->rcx, f->rdx, f->rsi, f->rdi, orig_nr(t), f->rip, f->cs, f->rflags,
                       f->rsp, f->ss, t->arch.fs_base, 0, 0, 0, 0, 0 };
    memcpy(r, v, sizeof v);
    return PRSTATUS_SIZE;
}
static bool user_addr_ok(uint64_t a) { return a < 0x800000000000ULL; }
static int regs_set(struct thread *t, const uint64_t *r) {
    struct trap_frame *f = thread_user_frame(t);
    if (!user_addr_ok(r[16]) || !user_addr_ok(r[21])) return -EIO;
    f->r15 = r[0]; f->r14 = r[1]; f->r13 = r[2]; f->r12 = r[3]; f->rbp = r[4]; f->rbx = r[5];
    f->r11 = r[6]; f->r10 = r[7]; f->r9 = r[8]; f->r8 = r[9]; f->rax = r[10]; f->rcx = r[11];
    f->rdx = r[12]; f->rsi = r[13]; f->rdi = r[14];
    if (t->pt_why == PT_STOP_SC_ENTRY) t->pt_sc_nr = (int)r[15];
    f->rip = r[16];
    const uint64_t user_flags = 0x50dd5;           /* CF PF AF ZF SF TF DF OF RF AC */
    f->rflags = (f->rflags & ~user_flags) | (r[18] & user_flags) | 0x202;
    f->rsp = r[19];
    arch_set_tls(t, r[21]);
    return 0;
}
static int fpregs_get(struct thread *t, void *buf) { memcpy(buf, t->arch.fpu, 512); return 512; }
static int fpregs_set(struct thread *t, const void *buf) {
    memcpy(t->arch.fpu, buf, 512);
    *(uint32_t *)(t->arch.fpu + 24) &= 0xffbf;      /* MXCSR: keep reserved bits clear (no #GP on fxrstor) */
    *(uint32_t *)(t->arch.fpu + 28) = 0xffff;       /* MXCSR_MASK as saved by fxsave */
    return 0;
}
static void sc_args(struct trap_frame *f, uint64_t *a) {
    a[0] = f->rdi; a[1] = f->rsi; a[2] = f->rdx; a[3] = f->r10; a[4] = f->r8; a[5] = f->r9;
}
static void sc_entry_prepare(struct trap_frame *f) { f->rax = (uint64_t)-ENOSYS; }
static int64_t sc_entry_nr(struct thread *t, struct trap_frame *f) { (void)f; return t->pt_sc_nr; }
#elif defined(__aarch64__)
#include <arch/cpu.h>
#define ARCH_HAS_STEP 1
#define AUDIT_ARCH 0xc00000b7u
#define PRSTATUS_SIZE (34 * 8)
#define FPREGS_SIZE 528
#define SPSR_SS (1ULL << 21)
static void arch_step(struct thread *t, bool on) {
    struct trap_frame *f = thread_user_frame(t);
    t->pt_step = on;
    if (on) f->pstate |= SPSR_SS; else f->pstate &= ~SPSR_SS;
    if (t == current) {                 /* MDSCR_EL1.SS is per CPU: arch_switch_to() follows pt_step */
        uint64_t m = sysreg_read(mdscr_el1);
        sysreg_write(mdscr_el1, on ? m | 1 : m & ~1ULL);
        __asm__ volatile("isb" ::: "memory");
    }
}
/* user_pt_regs: x0-x30, sp, pc, pstate == the first 34 words of the trap frame */
static int regs_get(struct thread *t, uint64_t *r) { memcpy(r, thread_user_frame(t), PRSTATUS_SIZE); return PRSTATUS_SIZE; }
static int regs_set(struct thread *t, const uint64_t *r) {
    struct trap_frame *f = thread_user_frame(t);
    if (r[32] >= 0x1000000000000ULL) return -EIO;
    uint64_t ps = f->pstate;
    memcpy(f, r, 33 * 8);
    f->pstate = (ps & ~0xf0000000ULL) | (r[33] & 0xf0000000ULL);   /* NZCV only, EL0t stays */
    return 0;
}
/* user_fpsimd_state: vregs[32] (128-bit), u32 fpsr, u32 fpcr, u32 reserved[2] */
static int fpregs_get(struct thread *t, void *buf) {
    memcpy(buf, t->arch.fpu, 512);
    uint32_t w[4] = { (uint32_t)t->arch.fpu[64], (uint32_t)t->arch.fpu[65], 0, 0 };
    memcpy((uint8_t *)buf + 512, w, 16);
    return FPREGS_SIZE;
}
static int fpregs_set(struct thread *t, const void *buf) {
    uint32_t w[4];
    memcpy(t->arch.fpu, buf, 512);
    memcpy(w, (const uint8_t *)buf + 512, 16);
    t->arch.fpu[64] = w[0]; t->arch.fpu[65] = w[1];
    return 0;
}
static void sc_args(struct trap_frame *f, uint64_t *a) {
    a[0] = f->orig_x0; for (int i = 1; i < 6; i++) a[i] = f->regs[i];
}
static void sc_entry_prepare(struct trap_frame *f) { (void)f; }
static int64_t sc_entry_nr(struct thread *t, struct trap_frame *f) { f->orig_x0 = f->regs[0]; return t->pt_sc_nr; }
#else /* riscv64: no hardware single step for U-mode, so the kernel steps in software: decode the
       * instruction at pc, plant c.ebreak at every possible successor (both sides of a branch),
       * and take them out again at the next stop (see ptrace_step_trap). */
#define ARCH_HAS_STEP 1
#define AUDIT_ARCH 0xc00000f3u
#define PRSTATUS_SIZE (32 * 8)
#define FPREGS_SIZE (33 * 8)
#define C_EBREAK 0x9002
static int64_t sext(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }
static uint64_t xreg(struct trap_frame *f, unsigned r) { return r ? f->regs[r] : 0; }
/* possible next PCs of the instruction at pc: returns how many (1 or 2) */
static int next_pcs(struct trap_frame *f, uint64_t pc, uint32_t in, uint64_t *out) {
    if ((in & 3) == 3) {
        unsigned op = in & 0x7f;
        out[0] = pc + 4;
        if (op == 0x6f) {                                   /* jal */
            uint64_t imm = ((in >> 31) & 1) << 20 | ((in >> 21) & 0x3ff) << 1 | ((in >> 20) & 1) << 11 | ((in >> 12) & 0xff) << 12;
            out[0] = pc + sext(imm, 21); return 1;
        }
        if (op == 0x67) { out[0] = (xreg(f, (in >> 15) & 31) + sext(in >> 20, 12)) & ~1ULL; return 1; }   /* jalr */
        if (op == 0x63) {                                   /* branches */
            uint64_t imm = ((in >> 31) & 1) << 12 | ((in >> 25) & 0x3f) << 5 | ((in >> 8) & 0xf) << 1 | ((in >> 7) & 1) << 11;
            out[1] = pc + sext(imm, 13); return out[1] == out[0] ? 1 : 2;
        }
        return 1;
    }
    in &= 0xffff;
    unsigned q = in & 3, f3 = in >> 13;
    out[0] = pc + 2;
    if (q == 1 && f3 == 5) {                                /* c.j */
        uint64_t imm = ((in >> 12) & 1) << 11 | ((in >> 11) & 1) << 4 | ((in >> 9) & 3) << 8 | ((in >> 8) & 1) << 10 |
                       ((in >> 7) & 1) << 6 | ((in >> 6) & 1) << 7 | ((in >> 3) & 7) << 1 | ((in >> 2) & 1) << 5;
        out[0] = pc + sext(imm, 12); return 1;
    }
    if (q == 1 && (f3 == 6 || f3 == 7)) {                   /* c.beqz / c.bnez */
        uint64_t imm = ((in >> 12) & 1) << 8 | ((in >> 10) & 3) << 3 | ((in >> 5) & 3) << 6 | ((in >> 3) & 3) << 1 | ((in >> 2) & 1) << 5;
        out[1] = pc + sext(imm, 9); return out[1] == out[0] ? 1 : 2;
    }
    if (q == 2 && f3 == 4 && !((in >> 2) & 31) && ((in >> 7) & 31)) {   /* c.jr / c.jalr */
        out[0] = xreg(f, (in >> 7) & 31) & ~1ULL; return 1;
    }
    return 1;
}
static void step_remove(struct thread *t) {
    struct mm *mm = t->proc ? t->proc->mm : nullptr;
    for (int i = t->pt_ss_n - 1; i >= 0; i--)
        if (mm) mm_write(mm, t->pt_ss_addr[i], &t->pt_ss_orig[i], 2);
    t->pt_ss_n = 0;
}
static void arch_step(struct thread *t, bool on) {
    struct trap_frame *f = thread_user_frame(t);
    struct mm *mm = t->proc ? t->proc->mm : nullptr;
    step_remove(t);
    t->pt_step = on;
    if (!on || !mm) return;
    uint32_t in = 0;
    uint16_t lo, hi = 0;
    if (mm_read(mm, f->sepc, &lo, 2)) return;
    if ((lo & 3) == 3 && mm_read(mm, f->sepc + 2, &hi, 2)) return;
    in = lo | (uint32_t)hi << 16;
    uint64_t nx[2];
    int n = next_pcs(f, f->sepc, in, nx);
    for (int i = 0; i < n; i++) {
        uint16_t orig, eb = C_EBREAK;
        if (nx[i] == f->sepc || (nx[i] & 1) || nx[i] >= 0x800000000000ULL) continue;
        if (i == 1 && t->pt_ss_n && t->pt_ss_addr[0] == nx[i]) continue;
        if (mm_read(mm, nx[i], &orig, 2) || mm_write(mm, nx[i], &eb, 2)) continue;
        t->pt_ss_addr[t->pt_ss_n] = nx[i]; t->pt_ss_orig[t->pt_ss_n] = orig; t->pt_ss_n++;
    }
}
/* ebreak trap from user mode at pc: one of our step breakpoints? (then it is a TRAP_TRACE) */
bool ptrace_step_trap(struct thread *t, uint64_t pc) {
    for (int i = 0; i < t->pt_ss_n; i++)
        if (t->pt_ss_addr[i] == pc) { step_remove(t); t->pt_step = false; return true; }
    return false;
}
/* user_regs_struct: pc, ra, sp, gp, tp, t0-t2, s0-s1, a0-a7, s2-s11, t3-t6 == pc + x1..x31 */
static int regs_get(struct thread *t, uint64_t *r) {
    struct trap_frame *f = thread_user_frame(t);
    r[0] = f->sepc;
    memcpy(r + 1, &f->regs[1], 31 * 8);
    return PRSTATUS_SIZE;
}
static int regs_set(struct thread *t, const uint64_t *r) {
    struct trap_frame *f = thread_user_frame(t);
    if (r[0] >= 0x800000000000ULL) return -EIO;
    f->sepc = r[0];
    memcpy(&f->regs[1], r + 1, 31 * 8);
    return 0;
}
static int fpregs_get(struct thread *t, void *buf) { memcpy(buf, t->arch.fpu, 33 * 8); return FPREGS_SIZE; }
static int fpregs_set(struct thread *t, const void *buf) { memcpy(t->arch.fpu, buf, 33 * 8); return 0; }
static void sc_args(struct trap_frame *f, uint64_t *a) {
    a[0] = f->orig_a0; for (int i = 1; i < 6; i++) a[i] = f->regs[10 + i];
}
static void sc_entry_prepare(struct trap_frame *f) { (void)f; }
/* the tracer changes the syscall number through a7 */
static int64_t sc_entry_nr(struct thread *t, struct trap_frame *f) {
    (void)t; f->orig_a0 = f->regs[10]; return (int64_t)f->regs[17];
}
#endif

#if defined(__riscv)
#define ARCH_STEP_SYSCALL_REPORT 0      /* the c.ebreak after the ecall reports the step */
#else
#define ARCH_STEP_SYSCALL_REPORT 1
#endif

static void icache_sync(void) {
#if defined(__aarch64__)
    __asm__ volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory");
#endif
    /* riscv64 executes fence.i on every entry to user mode (trap.S); x86 is coherent */
}

/* ---------------------------------------------------------------- attach / detach */

static void attach(struct thread *t, struct process *tracer, bool seized, uint32_t opts) {
    t->ptracer = tracer;
    list_add_tail(&tracer->tracees, &t->ptrace_node);
    t->pt_seized = seized;
    t->pt_opts = opts;
    t->pt_mode = PTRACE_CONT;
    t->pt_stopped = t->pt_reported = t->pt_interrupt = t->pt_listen = false;
    t->pt_why = 0;
}

static void resume(struct thread *t) {
    t->pt_stopped = false;
    t->pt_listen = false;
    wake_up(&pt_wq);
}

void arch_hw_debug_reset(struct thread *t);
static void untrace(struct thread *t) {
    if (!t->ptracer) return;
    list_del(&t->ptrace_node);
    t->ptracer = nullptr;
    t->pt_opts = 0;
    t->pt_mode = 0;
    t->pt_seized = t->pt_interrupt = false;
    arch_step(t, false);
    arch_hw_debug_reset(t);
    if (t->pt_stopped) resume(t);
}

static bool may_attach(struct process *p) {
    if (capable(CAP_SYS_PTRACE)) return true;
    const struct cred *c = current_cred();
    struct cred *tc = proc_cred(p);
    bool ok = c->uid == tc->uid && c->uid == tc->euid && c->uid == tc->suid &&
              c->gid == tc->gid && c->gid == tc->egid && c->gid == tc->sgid &&
              (tc->cap_perm & ~c->cap_perm) == 0;
    cred_put(tc);
    return ok;
}

/* ---------------------------------------------------------------- tracee side: stops */

static void notify_tracer(struct thread *t) {
    struct process *tr = t->ptracer;
    if (!tr) return;
    if (!(tr->sigactions[SIGCHLD].flags & SA_NOCLDSTOP)) {
        struct ksiginfo ki;
        memset(&ki, 0, sizeof ki);
        ki.signo = SIGCHLD; ki.code = CLD_TRAPPED;
        ki.pid = t->tid;
        ki.uid = t->proc && t->proc->cred ? t->proc->cred->uid : 0;
        ki.i1 = (t->pt_status >> 8) & 0xff;
        signal_send_info(tr, &ki);
    }
    wake_up(&tr->child_wait);
}

/* Enter a ptrace stop; returns false if none happened (not traced / being killed). */
static bool pt_stop(struct thread *t, int why, int status) {
    if (!t->ptracer || fatal(t)) return false;
    arch_step(t, false);
    t->pt_why = why;
    t->pt_status = status;
    t->pt_data = 0;
    t->pt_reported = false;
    t->pt_stopped = true;
    notify_tracer(t);
    for (;;) {
        uint64_t f = sched_wait_lock();
        if (!t->pt_stopped || fatal(t)) { sched_wait_unlock(f); break; }
        wait_event_uninterruptible_locked(&pt_wq, f);
    }
    t->pt_stopped = false;
    t->pt_why = 0;
    if (t->ptracer && t->pt_mode == PTRACE_SINGLESTEP && !fatal(t)) arch_step(t, true);
    return true;
}

static void si_trap(struct thread *t, int code) {
    memset(&t->pt_si, 0, sizeof t->pt_si);
    t->pt_si.signo = SIGTRAP;
    t->pt_si.code = code;
}

static void pt_event(struct thread *t, int ev, uint64_t msg, int sig) {
    t->pt_msg = msg;
    si_trap(t, sig | (ev << 8));
    pt_stop(t, ev == PTRACE_EVENT_STOP ? PT_STOP_INTERRUPT : PT_STOP_EVENT, ((sig | (ev << 8)) << 8) | 0x7f);
}

/* signal-delivery-stop: returns the signal to deliver (0 = suppressed); *ki is updated */
int ptrace_signal_stop(struct trap_frame *f, struct ksiginfo *ki) {
    (void)f;
    struct thread *t = current;
    int sig = ki->signo;
    t->pt_si = *ki;
    if (!pt_stop(t, PT_STOP_SIGNAL, (sig << 8) | 0x7f)) return t->ptracer || fatal(t) ? 0 : sig;
    sig = t->pt_data;
    if (!sig || sig >= NSIG) return 0;
    if (t->pt_si.signo != sig) {            /* replaced signal: SI_USER from the tracer */
        struct ksiginfo n;
        memset(&n, 0, sizeof n);
        n.signo = sig; n.code = SI_USER;
        n.pid = t->ptracer ? t->ptracer->pid : 0;
        t->pt_si = n;
    }
    *ki = t->pt_si;
    return sig;
}

/* default action of a stop signal for a tracee: a ptrace stop instead of the job-control stop */
bool ptrace_group_stop(int sig) {
    struct thread *t = current;
    if (!t->ptracer) return false;
    if (t->pt_seized) { si_trap(t, sig | (PTRACE_EVENT_STOP << 8)); t->pt_si.signo = sig; }
    int st = t->pt_seized ? ((sig | (PTRACE_EVENT_STOP << 8)) << 8) | 0x7f : (sig << 8) | 0x7f;
    pt_stop(t, PT_STOP_GROUP, st);
    return true;
}

/* PTRACE_INTERRUPT (and the initial stop of auto-attached seized children) */
void ptrace_interrupt_stop(void) {
    struct thread *t = current;
    t->pt_interrupt = false;
    if (t->ptracer && t->pt_seized) pt_event(t, PTRACE_EVENT_STOP, 0, SIGTRAP);
}

void ptrace_kill_wake(struct process *p) { (void)p; wake_up(&pt_wq); }

/* SIGCONT for a process with PTRACE_LISTEN tracees: they re-trap with PTRACE_EVENT_STOP */
void ptrace_cont_notify(struct process *p) {
    list_for_each(it, &p->threads) {
        struct thread *t = list_entry(it, struct thread, proc_node);
        if (!t->ptracer || !t->pt_listen || !t->pt_stopped) continue;
        t->pt_listen = false;
        t->pt_why = PT_STOP_INTERRUPT;
        t->pt_status = ((SIGTRAP | (PTRACE_EVENT_STOP << 8)) << 8) | 0x7f;
        si_trap(t, SIGTRAP | (PTRACE_EVENT_STOP << 8));
        t->pt_reported = false;
        notify_tracer(t);
    }
}

/* syscall entry: returns the (possibly changed) syscall number, or -1 to skip the call */
int64_t ptrace_syscall_entry(struct trap_frame *f) {
    struct thread *t = current;
    t->pt_sc_nr = (int)SC_NR(f);
    if (!t->ptracer || t->pt_mode != PTRACE_SYSCALL) return (int64_t)SC_NR(f);
    int sig = SIGTRAP | ((t->pt_opts & PTRACE_O_TRACESYSGOOD) ? 0x80 : 0);
    sc_entry_prepare(f);
    si_trap(t, sig);
    if (!pt_stop(t, PT_STOP_SC_ENTRY, (sig << 8) | 0x7f)) return fatal(t) ? -1 : t->pt_sc_nr;
    if (fatal(t)) return -1;
    int64_t nr = sc_entry_nr(t, f);
    t->pt_sc_nr = (int)nr;
    return (int)nr;
}

void ptrace_syscall_exit(struct trap_frame *f) {
    struct thread *t = current;
    if (!t->ptracer) return;
    if (t->pt_mode == PTRACE_SYSCALL) {
        int sig = SIGTRAP | ((t->pt_opts & PTRACE_O_TRACESYSGOOD) ? 0x80 : 0);
        si_trap(t, sig);
        pt_stop(t, PT_STOP_SC_EXIT, (sig << 8) | 0x7f);
    } else if (t->pt_mode == PTRACE_SINGLESTEP && ARCH_STEP_SYSCALL_REPORT) {
        /* the stepped instruction was the syscall: report the step now, like Linux */
        arch_step(t, false);
        signal_force_info(t, SIGTRAP, TRAP_TRACE, FRAME_PC(f));
    }
}

static int fork_event(uint64_t flags, int exit_signal) {
    if (flags & CLONE_VFORK) return PTRACE_EVENT_VFORK;
    if (exit_signal != SIGCHLD) return PTRACE_EVENT_CLONE;
    return PTRACE_EVENT_FORK;
}

/* the new thread/process of a traced parent (before it was started) */
void ptrace_fork_attach(struct thread *child, uint64_t flags, int exit_signal) {
    struct thread *t = current;
    int ev = fork_event(flags, exit_signal);
    if (!t->ptracer || !(t->pt_opts & (1u << ev))) return;
    attach(child, t->ptracer, t->pt_seized, t->pt_opts);
    if (child->pt_seized) child->pt_interrupt = true;               /* PTRACE_EVENT_STOP first */
    else signal_thread_queue(child, SIGSTOP, SI_USER, false);        /* classic: SIGSTOP stop first */
}

void ptrace_fork_event(uint64_t flags, int exit_signal, int child_tid) {
    struct thread *t = current;
    int ev = fork_event(flags, exit_signal);
    if (t->ptracer && (t->pt_opts & (1u << ev))) pt_event(t, ev, (uint64_t)child_tid, SIGTRAP);
}

void ptrace_vfork_done(int child_tid) {
    struct thread *t = current;
    if (t->ptracer && (t->pt_opts & PTRACE_O_TRACEVFORKDONE))
        pt_event(t, PTRACE_EVENT_VFORK_DONE, (uint64_t)child_tid, SIGTRAP);
}

void ptrace_exec_event(const char *path) {
    (void)path;
    struct thread *t = current;
    if (!t->ptracer) return;
    if (t->pt_opts & PTRACE_O_TRACEEXEC) pt_event(t, PTRACE_EVENT_EXEC, (uint64_t)t->tid, SIGTRAP);
    else if (!t->pt_seized) signal_thread_queue(t, SIGTRAP, SI_USER, false);   /* legacy post-exec SIGTRAP */
}

void ptrace_exit_event(int status) {
    struct thread *t = current;
    if (t->ptracer && (t->pt_opts & PTRACE_O_TRACEEXIT)) pt_event(t, PTRACE_EVENT_EXIT, (uint64_t)(uint32_t)status, SIGTRAP);
}

/* the traced thread t (of process p) is gone */
void ptrace_thread_gone(struct thread *t, struct process *p, int status) {
    struct process *tr = t->ptracer;
    if (!tr) return;
    bool leader = p && t->tid == p->pid;
    bool child = p && p->parent == tr;
    untrace(t);
    if (leader && child) return;            /* the normal zombie reaping reports it */
    struct pt_exit *e = kmalloc(sizeof *e);
    if (!e) return;
    e->tid = t->tid;
    e->pgid = p ? p->pgid : 0;
    e->status = status;
    e->clone = !leader || !p || p->exit_signal != SIGCHLD;
    list_add_tail(&tr->pt_exits, &e->node);
    wake_up(&tr->child_wait);
}

/* the tracing process p exits: detach (or kill, PTRACE_O_EXITKILL) its tracees */
void ptrace_tracer_exit(struct process *p) {
    if (!p->tracees.next) return;
    list_for_each_safe(it, tmp, &p->tracees) {
        struct thread *t = list_entry(it, struct thread, ptrace_node);
        bool kill = t->pt_opts & PTRACE_O_EXITKILL;
        struct process *tp = t->proc;
        untrace(t);
        if (kill && tp) signal_send(tp, SIGKILL);
    }
    list_for_each_safe(it, tmp, &p->pt_exits) {
        list_del(it);
        kfree(list_entry(it, struct pt_exit, node));
    }
}

/* ---------------------------------------------------------------- tracer side: wait */

static bool pt_match(int tid, int pgid, bool clone, int idtype, int id, int options, struct process *self) {
    if (idtype == 1 && tid != id) return false;
    if (idtype == 2 && pgid != (id ? id : self->pgid)) return false;
    if (!(options & __WALL) && clone != !!(options & __WCLONE)) return false;
    return true;
}
static bool is_clone(struct thread *t) {
    return !t->proc || t->tid != t->proc->pid || t->proc->exit_signal != SIGCHLD;
}

int ptrace_wait(struct process *self, int idtype, int id, int options, struct wait_result *res) {
    if (!self->tracees.next) return 0;
    if (options & WEXITED) {
        list_for_each_safe(it, tmp, &self->pt_exits) {
            struct pt_exit *e = list_entry(it, struct pt_exit, node);
            if (!pt_match(e->tid, e->pgid, e->clone, idtype, id, options, self)) continue;
            int st = e->status;
            memset(res, 0, sizeof *res);
            res->pid = e->tid;
            res->status = st;
            res->code = (st & 0x7f) == 0 ? CLD_EXITED : (st & 0x80) ? CLD_DUMPED : CLD_KILLED;
            int tid = e->tid;
            if (!(options & WNOWAIT)) { list_del(&e->node); kfree(e); }
            return tid;
        }
    }
    list_for_each(it, &self->tracees) {
        struct thread *t = list_entry(it, struct thread, ptrace_node);
        if (!t->pt_stopped || t->pt_reported) continue;
        if (!pt_match(t->tid, t->proc ? t->proc->pgid : 0, is_clone(t), idtype, id, options, self)) continue;
        memset(res, 0, sizeof *res);
        res->pid = t->tid;
        res->status = t->pt_status;
        res->code = CLD_TRAPPED;
        res->uid = t->proc && t->proc->cred ? t->proc->cred->uid : 0;
        if (!(options & WNOWAIT)) t->pt_reported = true;
        return t->tid;
    }
    return 0;
}

bool ptrace_has_tracees(struct process *self, int idtype, int id) {
    if (!self->tracees.next) return false;
    list_for_each(it, &self->tracees) {
        struct thread *t = list_entry(it, struct thread, ptrace_node);
        if (pt_match(t->tid, t->proc ? t->proc->pgid : 0, false, idtype, id, __WALL, self)) return true;
    }
    list_for_each(it, &self->pt_exits) {
        struct pt_exit *e = list_entry(it, struct pt_exit, node);
        if (pt_match(e->tid, e->pgid, false, idtype, id, __WALL, self)) return true;
    }
    return false;
}

/* ---------------------------------------------------------------- tracer side: requests */

struct pt_iovec { uint64_t base, len; };

#define NT_ARM_HW_BREAK 0x402
#define NT_ARM_HW_WATCH 0x403
#if defined(__aarch64__)
/* struct user_hwdebug_state: u32 dbg_info, u32 pad, { u64 addr; u32 ctrl; u32 pad; } regs[16].
 * ctrl uses the DBGBCR/DBGWCR layout: E (bit 0), privilege (2:1, only EL0 = 2), LSC (4:3,
 * watchpoints: 1 load, 2 store, 3 both), byte address select (12:5). */
int a64_num_brps(void), a64_num_wrps(void);
static size_t hwdebug_get(struct thread *t, bool watch, uint8_t *buf) {
    int n = watch ? a64_num_wrps() : a64_num_brps();
    memset(buf, 0, 8 + 16 * 16);
    uint32_t info = (6u << 8) | (uint32_t)n;           /* debug architecture ARMv8 */
    memcpy(buf, &info, 4);
    for (int i = 0; i < n; i++) {
        uint64_t a = watch ? t->arch.wvr[i] : t->arch.bvr[i];
        uint32_t c = watch ? t->arch.wcr[i] : t->arch.bcr[i];
        memcpy(buf + 8 + 16 * i, &a, 8);
        memcpy(buf + 16 + 16 * i, &c, 4);
    }
    return 8 + 16 * (size_t)n;
}
static int hwdebug_set(struct thread *t, bool watch, const uint8_t *buf, size_t len) {
    int n = watch ? a64_num_wrps() : a64_num_brps();
    if (len < 8) return 0;
    size_t cnt = (len - 8) / 16;
    if (cnt > (size_t)n) return -ENOSPC;
    uint64_t addr[16]; uint32_t ctrl[16];
    for (size_t i = 0; i < cnt; i++) {
        memcpy(&addr[i], buf + 8 + 16 * i, 8);
        memcpy(&ctrl[i], buf + 16 + 16 * i, 4);
        uint32_t c = ctrl[i];
        if (!(c & 1)) { ctrl[i] = c & 0x1ffe; continue; }      /* disabled: kept, not armed */
        if (((c >> 1) & 3) != 2) return -EINVAL;               /* EL0 only */
        if (addr[i] >= 0x1000000000000ULL) return -EINVAL;
        uint32_t bas = (c >> 5) & 0xff;
        if (watch) {
            if (!((c >> 3) & 3) || !bas || (addr[i] & 7)) return -EINVAL;
            ctrl[i] = c & 0x1fff;
        } else {
            if (addr[i] & 3) return -EINVAL;
            ctrl[i] = (c & 7) | (0xf << 5);                    /* A64 instructions: BAS = 0b1111 */
        }
    }
    for (size_t i = 0; i < cnt; i++) {
        if (watch) { t->arch.wvr[i] = addr[i]; t->arch.wcr[i] = ctrl[i]; }
        else { t->arch.bvr[i] = addr[i]; t->arch.bcr[i] = ctrl[i]; }
    }
    bool any = false;
    for (int i = 0; i < 16; i++) any |= (t->arch.bcr[i] | t->arch.wcr[i]) & 1;
    t->arch.hwdbg = any;
    return 0;
}
#endif

static int64_t regset(struct thread *t, bool set, int type, struct pt_iovec *uiov) {
    struct pt_iovec iov;
    if (copy_from_user(&iov, uiov, sizeof iov)) return -EFAULT;
    uint8_t buf[528] __attribute__((aligned(16)));
    size_t size;
    switch (type) {
    case NT_PRSTATUS: size = PRSTATUS_SIZE; regs_get(t, (uint64_t *)buf); break;
    case NT_PRFPREG: size = FPREGS_SIZE; fpregs_get(t, buf); break;
#if defined(__aarch64__)
    case NT_ARM_TLS: size = 8; memcpy(buf, &t->arch.tpidr, 8); break;
    case NT_ARM_SYSTEM_CALL: { size = 4; int32_t n = t->pt_sc_nr; memcpy(buf, &n, 4); break; }
    case NT_ARM_HW_BREAK: case NT_ARM_HW_WATCH: size = hwdebug_get(t, type == NT_ARM_HW_WATCH, buf); break;
#endif
    default: return -EINVAL;
    }
    size_t n = MIN(iov.len, size);
    if (!set) {
        if (copy_to_user((void *)iov.base, buf, n)) return -EFAULT;
    } else {
        if (n < size && type != NT_PRSTATUS && type != NT_PRFPREG && type != NT_ARM_HW_BREAK && type != NT_ARM_HW_WATCH) return -EINVAL;
        if (copy_from_user(buf, (void *)iov.base, n)) return -EFAULT;   /* partial writes keep the rest */
        int r = 0;
        switch (type) {
        case NT_PRSTATUS: r = regs_set(t, (uint64_t *)buf); break;
        case NT_PRFPREG: r = fpregs_set(t, buf); break;
#if defined(__aarch64__)
        case NT_ARM_TLS: memcpy(&t->arch.tpidr, buf, 8); break;
        case NT_ARM_SYSTEM_CALL: { int32_t v; memcpy(&v, buf, 4); t->pt_sc_nr = v; break; }
        case NT_ARM_HW_BREAK: case NT_ARM_HW_WATCH: r = hwdebug_set(t, type == NT_ARM_HW_WATCH, buf, n); break;
#endif
        }
        if (r) return r;
    }
    iov.len = n;
    return copy_to_user(&uiov->len, &iov.len, sizeof iov.len) ? -EFAULT : 0;
}

/* struct ptrace_syscall_info */
struct pt_sci {
    uint8_t op, pad[3];
    uint32_t arch;
    uint64_t ip, sp;
    union {
        struct { uint64_t nr, args[6]; } entry;
        struct { int64_t rval; uint8_t is_error; } exit;
    };
};

static int64_t syscall_info(struct thread *t, size_t usize, void *ubuf) {
    struct trap_frame *f = thread_user_frame(t);
    struct pt_sci si;
    memset(&si, 0, sizeof si);
    si.arch = AUDIT_ARCH;
    si.ip = FRAME_PC(f);
    si.sp = FRAME_SP(f);
    size_t size = 24;
    if (t->pt_why == PT_STOP_SC_ENTRY) {
        si.op = 1;
        si.entry.nr = (uint64_t)(int64_t)t->pt_sc_nr;
        sc_args(f, si.entry.args);
        size = 24 + 56;
    } else if (t->pt_why == PT_STOP_SC_EXIT) {
        si.op = 2;
        si.exit.rval = (int64_t)SC_RET(f);
        si.exit.is_error = si.exit.rval < 0 && si.exit.rval >= -4095;
        size = 24 + 9;
    }
    if (copy_to_user(ubuf, &si, MIN(usize, size))) return -EFAULT;
    return (int64_t)size;
}

static int64_t peekuser(struct thread *t, uint64_t off, uint64_t *out) {
#if defined(__x86_64__)
    if (off & 7) return -EIO;
    if (off < PRSTATUS_SIZE) { uint64_t r[27]; regs_get(t, r); *out = r[off / 8]; return 0; }
    if (off >= 848 && off < 848 + 64) {              /* u_debugreg[8] */
        int i = (int)(off - 848) / 8;
        *out = i < 4 ? t->arch.dr[i] : i == 6 ? t->arch.dr6 : i == 7 ? t->arch.dr7 : 0;
        return 0;
    }
    if (off < 928) { *out = 0; return 0; }           /* rest of struct user: not used */
#endif
    (void)t; (void)off; (void)out;
    return -EIO;
}
static int64_t pokeuser(struct thread *t, uint64_t off, uint64_t v) {
#if defined(__x86_64__)
    if (off & 7) return -EIO;
    if (off < PRSTATUS_SIZE) {
        uint64_t r[27];
        regs_get(t, r);
        r[off / 8] = v;
        return regs_set(t, r);
    }
    if (off >= 848 && off < 848 + 64) {              /* u_debugreg: DR0-3 addresses, DR6, DR7 */
        int i = (int)(off - 848) / 8;
        if (i < 4) {
            if (v > 0x800000000000ULL - 8) return -EIO;
            t->arch.dr[i] = v;
            return 0;
        }
        if (i == 6) { t->arch.dr6 = v; return 0; }
        if (i != 7) return v ? -EIO : 0;
        if (v & ~0xffff03ffULL) return -EIO;          /* GD and reserved bits */
        uint64_t d7 = 0;
        for (int n = 0; n < 4; n++) {
            uint64_t en = (v >> (2 * n)) & 3, rw = (v >> (16 + 4 * n)) & 3, len = (v >> (18 + 4 * n)) & 3;
            if (!en) continue;
            if (rw == 2) return -EIO;                    /* I/O breakpoints */
            if (rw == 0 && len) return -EIO;             /* execute: length 1 */
            uint64_t sz = len == 0 ? 1 : len == 1 ? 2 : len == 3 ? 4 : 8;
            if (t->arch.dr[n] & (sz - 1)) return -EIO;   /* naturally aligned */
            d7 |= (1ULL << (2 * n)) | (rw << (16 + 4 * n)) | (len << (18 + 4 * n));   /* local enable */
        }
        t->arch.dr7 = d7 ? d7 | 0x100 : 0;               /* LE */
        return 0;
    }
#endif
    (void)t; (void)off; (void)v;
    return -EIO;
}

static int64_t do_attach(int64_t req, int pid, uint64_t addr, uint64_t data) {
    struct thread *t = process_find_thread(pid);
    if (!t || !t->proc || t->proc->state == P_ZOMBIE) return -ESRCH;
    if (t->proc == curproc || t->ptracer || t->proc->pid == 1) return -EPERM;
    if (!may_attach(t->proc)) return -EPERM;
    if (req == PTRACE_SEIZE) {
        if (addr) return -EIO;
        if (data & ~(uint64_t)PTRACE_O_MASK) return -EINVAL;
        attach(t, curproc, true, (uint32_t)data);
        return 0;
    }
    attach(t, curproc, false, 0);
    signal_thread_queue(t, SIGSTOP, SI_USER, true);
    return 0;
}

int64_t sys_ptrace(int64_t req, int64_t pid, uint64_t addr, uint64_t data) {
    struct thread *self = current;
    if (req == PTRACE_TRACEME) {
        struct process *par = curproc->parent;
        if (self->ptracer || !par) return -EPERM;
        attach(self, par, false, 0);
        return 0;
    }
    if (req == PTRACE_ATTACH || req == PTRACE_SEIZE) return do_attach(req, (int)pid, addr, data);
    struct thread *t = process_find_thread((int)pid);
    if (!t || t->ptracer != curproc || !t->proc) return -ESRCH;
    if (req == PTRACE_KILL) { signal_send(t->proc, SIGKILL); return 0; }
    if (req == PTRACE_INTERRUPT) {
        if (!t->pt_seized) return -EIO;
        if (t->pt_stopped && t->pt_listen) {             /* listening: re-trap now */
            t->pt_listen = false;
            t->pt_why = PT_STOP_INTERRUPT;
            t->pt_status = ((SIGTRAP | (PTRACE_EVENT_STOP << 8)) << 8) | 0x7f;
            si_trap(t, SIGTRAP | (PTRACE_EVENT_STOP << 8));
            t->pt_reported = false;
            notify_tracer(t);
        } else if (!t->pt_stopped) {
            t->pt_interrupt = true;
            thread_interrupt(t);
        }
        return 0;
    }
    if (!t->pt_stopped || t->pt_listen) return -ESRCH;
    struct mm *mm = t->proc->mm;
    switch (req) {
    case PTRACE_PEEKTEXT: case PTRACE_PEEKDATA: {
        uint64_t w;
        if (!mm || mm_read(mm, addr, &w, 8)) return -EIO;
        return copy_to_user((void *)data, &w, 8) ? -EFAULT : 0;
    }
    case PTRACE_POKETEXT: case PTRACE_POKEDATA:
        if (!mm || mm_write(mm, addr, &data, 8)) return -EIO;
        icache_sync();
        return 0;
    case PTRACE_PEEKUSER: {
        uint64_t w;
        int64_t r = peekuser(t, addr, &w);
        if (r) return r;
        return copy_to_user((void *)data, &w, 8) ? -EFAULT : 0;
    }
    case PTRACE_POKEUSER: return pokeuser(t, addr, data);
    case PTRACE_SINGLESTEP:
        if (!ARCH_HAS_STEP) return -EIO;
        /* fall through */
    case PTRACE_CONT: case PTRACE_SYSCALL:
        if (data >= NSIG) return -EIO;
        t->pt_mode = (int)req;
        t->pt_data = (int)data;
        resume(t);
        return 0;
    case PTRACE_DETACH:
        if (data >= NSIG) return -EIO;
        t->pt_data = (int)data;
        untrace(t);
        return 0;
#if defined(__x86_64__)
    case PTRACE_GETREGS: { uint64_t r[27]; regs_get(t, r); return copy_to_user((void *)data, r, sizeof r) ? -EFAULT : 0; }
    case PTRACE_SETREGS: {
        uint64_t r[27];
        if (copy_from_user(r, (void *)data, sizeof r)) return -EFAULT;
        return regs_set(t, r);
    }
    case PTRACE_GETFPREGS: return copy_to_user((void *)data, t->arch.fpu, 512) ? -EFAULT : 0;
    case PTRACE_SETFPREGS: {
        uint8_t b[512] __attribute__((aligned(16)));
        if (copy_from_user(b, (void *)data, sizeof b)) return -EFAULT;
        return fpregs_set(t, b);
    }
#endif
    case PTRACE_GETREGSET: return regset(t, false, (int)addr, (struct pt_iovec *)data);
    case PTRACE_SETREGSET: return regset(t, true, (int)addr, (struct pt_iovec *)data);
    case PTRACE_SETOPTIONS:
        if (data & ~(uint64_t)PTRACE_O_MASK) return -EINVAL;
        t->pt_opts = (uint32_t)data;
        return 0;
    case PTRACE_GETEVENTMSG: return copy_to_user((void *)data, &t->pt_msg, 8) ? -EFAULT : 0;
    case PTRACE_GETSIGINFO: {
        uint8_t b[128];
        siginfo_to_user(&t->pt_si, b);
        return copy_to_user((void *)data, b, sizeof b) ? -EFAULT : 0;
    }
    case PTRACE_SETSIGINFO: {
        uint8_t b[128];
        if (copy_from_user(b, (void *)data, sizeof b)) return -EFAULT;
        siginfo_from_user(&t->pt_si, b);
        return 0;
    }
    case PTRACE_GETSIGMASK:
        if (addr != 8) return -EINVAL;
        return copy_to_user((void *)data, &t->sig_mask, 8) ? -EFAULT : 0;
    case PTRACE_SETSIGMASK: {
        uint64_t m;
        if (addr != 8) return -EINVAL;
        if (copy_from_user(&m, (void *)data, 8)) return -EFAULT;
        t->sig_mask = m & ~UNBLOCKABLE;
        return 0;
    }
    case PTRACE_LISTEN:
        if (!t->pt_seized || t->pt_why != PT_STOP_GROUP) return -EIO;
        t->pt_listen = true;
        return 0;
    case PTRACE_GET_SYSCALL_INFO: return syscall_info(t, addr, (void *)data);
    default: return -EIO;
    }
}
