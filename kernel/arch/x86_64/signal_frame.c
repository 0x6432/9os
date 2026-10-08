/* x86_64 signal frames (Linux rt_sigframe compatible layout). */
#include <kernel/process.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <arch/trapframe.h>
#include <arch/cpu.h>

struct sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2, fpstate, reserved[8];
};
struct ucontext {
    uint64_t uc_flags, uc_link;
    uint64_t ss_sp; int32_t ss_flags, _pad; uint64_t ss_size;
    struct sigcontext mc;
    uint64_t sigmask;
};
struct siginfo { int32_t signo, errno_, code, _pad; int32_t pid, uid, status; uint8_t rest[100]; };
struct rt_sigframe {
    uint64_t pretcode;
    struct ucontext uc;
    struct siginfo info;
    uint8_t fpu[512] __attribute__((aligned(16)));
};

int arch_setup_signal_frame(struct trap_frame *f, int sig, struct k_sigaction *ka, uint64_t oldmask,
                            const struct ksiginfo *ki) {
    struct thread *t = current;
    uint64_t sp = f->rsp;
    if ((ka->flags & SA_ONSTACK) && t->altstack_size && !(sp >= t->altstack_sp && sp < t->altstack_sp + t->altstack_size))
        sp = t->altstack_sp + t->altstack_size;
    sp -= 128;                                       /* red zone */
    sp -= sizeof(struct rt_sigframe);
    sp &= ~15ULL;
    sp -= 8;                                         /* so that (sp + 8) is 16-byte aligned at handler entry */
    struct rt_sigframe fr;
    memset(&fr, 0, sizeof fr);
    fr.pretcode = ka->restorer;
    struct sigcontext *mc = &fr.uc.mc;
    mc->r8 = f->r8; mc->r9 = f->r9; mc->r10 = f->r10; mc->r11 = f->r11; mc->r12 = f->r12;
    mc->r13 = f->r13; mc->r14 = f->r14; mc->r15 = f->r15; mc->rdi = f->rdi; mc->rsi = f->rsi;
    mc->rbp = f->rbp; mc->rbx = f->rbx; mc->rdx = f->rdx; mc->rax = f->rax; mc->rcx = f->rcx;
    mc->rsp = f->rsp; mc->rip = f->rip; mc->eflags = f->rflags; mc->cs = f->cs; mc->ss = f->ss;
    mc->trapno = f->vector; mc->err = f->error; mc->oldmask = oldmask; mc->cr2 = read_cr2();
    fr.uc.sigmask = oldmask;
    fr.uc.ss_sp = t->altstack_sp; fr.uc.ss_size = t->altstack_size; fr.uc.ss_flags = t->altstack_flags;
    __asm__ volatile("fxsave64 (%0)" :: "r"(fr.fpu) : "memory");
    struct rt_sigframe *u = (struct rt_sigframe *)sp;
    mc->fpstate = (uint64_t)u->fpu;
    siginfo_to_user(ki, &fr.info);
    if (copy_to_user(u, &fr, sizeof fr)) return -EFAULT;
    f->rsp = sp;
    f->rip = ka->handler;
    f->rdi = sig;
    f->rsi = (uint64_t)&u->info;
    f->rdx = (uint64_t)&u->uc;
    f->rax = 0;
    f->rflags &= ~0x400ULL;                          /* clear DF */
    return 0;
}

/* Returns the restored signal mask via *mask; frame is restored in place. */
int arch_sigreturn(struct trap_frame *f, uint64_t *mask) {
    struct rt_sigframe *u = (struct rt_sigframe *)(f->rsp - 8);   /* handler's ret popped pretcode */
    struct rt_sigframe fr;
    if (copy_from_user(&fr, u, sizeof fr)) return -EFAULT;
    struct sigcontext *mc = &fr.uc.mc;
    f->r8 = mc->r8; f->r9 = mc->r9; f->r10 = mc->r10; f->r11 = mc->r11; f->r12 = mc->r12;
    f->r13 = mc->r13; f->r14 = mc->r14; f->r15 = mc->r15; f->rdi = mc->rdi; f->rsi = mc->rsi;
    f->rbp = mc->rbp; f->rbx = mc->rbx; f->rdx = mc->rdx; f->rax = mc->rax; f->rcx = mc->rcx;
    f->rsp = mc->rsp; f->rip = mc->rip;
    f->rflags = (mc->eflags & 0xcd5) | 0x202;        /* user-modifiable flags only, IF forced */
    f->cs = 0x23; f->ss = 0x1b;
    fr.fpu[24] = fr.fpu[24];                         /* keep MXCSR as saved */
    *(uint32_t *)(fr.fpu + 24) &= 0xffbf;
    __asm__ volatile("fxrstor64 (%0)" :: "r"(fr.fpu) : "memory");
    *mask = fr.uc.sigmask;
    return 0;
}
