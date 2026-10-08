/* riscv64 signal frames (Linux rt_sigframe layout). */
#include <kernel/process.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/kmalloc.h>
#include <kernel/errno.h>
#include <arch/trapframe.h>

void fp_save(uint64_t *buf);
void fp_restore(const uint64_t *buf);

struct sigcontext {
    uint64_t regs[32];                                  /* [0] = pc, [i] = x<i> */
    uint64_t fpregs[66] __attribute__((aligned(16)));   /* union __riscv_fp_state (q-ext sized) */
};
struct ucontext {
    uint64_t uc_flags, uc_link;
    uint64_t ss_sp; int32_t ss_flags, _pad; uint64_t ss_size;
    uint64_t sigmask;
    uint8_t unused[120];
    struct sigcontext mc __attribute__((aligned(16)));
};
struct siginfo { int32_t signo, errno_, code, _pad; int32_t pid, uid, status; uint8_t rest[100]; };
struct rt_sigframe {
    struct siginfo info;
    struct ucontext uc;
};
_Static_assert(__builtin_offsetof(struct ucontext, mc) == 176, "ucontext layout");

int arch_setup_signal_frame(struct trap_frame *f, int sig, struct k_sigaction *ka, uint64_t oldmask,
                            const struct ksiginfo *ki) {
    struct thread *t = current;
    if (!t->proc->mm->sigtramp) return -EFAULT;
    uint64_t sp = f->regs[2];
    if ((ka->flags & SA_ONSTACK) && t->altstack_size && !(sp >= t->altstack_sp && sp < t->altstack_sp + t->altstack_size))
        sp = t->altstack_sp + t->altstack_size;
    sp -= sizeof(struct rt_sigframe);
    sp &= ~15ULL;
    struct rt_sigframe *fr = kzalloc(sizeof *fr);
    if (!fr) return -ENOMEM;
    siginfo_to_user(ki, &fr->info);
    fr->uc.sigmask = oldmask;
    fr->uc.ss_sp = t->altstack_sp; fr->uc.ss_size = t->altstack_size; fr->uc.ss_flags = t->altstack_flags;
    fr->uc.mc.regs[0] = f->sepc;
    for (int i = 1; i < 32; i++) fr->uc.mc.regs[i] = f->regs[i];
    fp_save(fr->uc.mc.fpregs);
    int r = copy_to_user((void *)sp, fr, sizeof *fr);
    kfree(fr);
    if (r) return -EFAULT;
    struct rt_sigframe *u = (struct rt_sigframe *)sp;
    f->regs[2] = sp;
    f->regs[1] = t->proc->mm->sigtramp;
    f->regs[10] = sig;
    f->regs[11] = (uint64_t)&u->info;
    f->regs[12] = (uint64_t)&u->uc;
    f->sepc = ka->handler;
    return 0;
}

int arch_sigreturn(struct trap_frame *f, uint64_t *mask) {
    struct rt_sigframe *fr = kmalloc(sizeof *fr);
    if (!fr) return -ENOMEM;
    if (copy_from_user(fr, (void *)f->regs[2], sizeof *fr)) { kfree(fr); return -EFAULT; }
    f->sepc = fr->uc.mc.regs[0];
    for (int i = 1; i < 32; i++) f->regs[i] = fr->uc.mc.regs[i];
    fp_restore(fr->uc.mc.fpregs);
    *mask = fr->uc.sigmask;
    kfree(fr);
    return 0;
}
