/* aarch64 signal frames (Linux rt_sigframe layout, with an fpsimd_context record). */
#include <kernel/process.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/kmalloc.h>
#include <kernel/errno.h>
#include <arch/trapframe.h>

void fp_save(uint64_t *buf);
void fp_restore(const uint64_t *buf);

#define FPSIMD_MAGIC 0x46508001
struct fpsimd_context { uint32_t magic, size; uint32_t fpsr, fpcr; uint64_t vregs[64]; };
struct sigcontext {
    uint64_t fault_address, regs[31], sp, pc, pstate;
    uint8_t reserved[4096] __attribute__((aligned(16)));
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
    uint64_t fp, lr;           /* frame record */
};
_Static_assert(__builtin_offsetof(struct ucontext, mc) == 176, "ucontext layout");

int arch_setup_signal_frame(struct trap_frame *f, int sig, struct k_sigaction *ka, uint64_t oldmask) {
    struct thread *t = current;
    uint64_t sp = f->sp;
    if ((ka->flags & SA_ONSTACK) && t->altstack_size && !(sp >= t->altstack_sp && sp < t->altstack_sp + t->altstack_size))
        sp = t->altstack_sp + t->altstack_size;
    sp -= sizeof(struct rt_sigframe);
    sp &= ~15ULL;
    struct rt_sigframe *fr = kzalloc(sizeof *fr);
    if (!fr) return -ENOMEM;
    fr->info.signo = sig;
    fr->uc.sigmask = oldmask;
    fr->uc.ss_sp = t->altstack_sp; fr->uc.ss_size = t->altstack_size; fr->uc.ss_flags = t->altstack_flags;
    struct sigcontext *mc = &fr->uc.mc;
    for (int i = 0; i < 31; i++) mc->regs[i] = f->regs[i];
    mc->sp = f->sp; mc->pc = f->pc; mc->pstate = f->pstate; mc->fault_address = f->far;
    struct fpsimd_context *fc = (struct fpsimd_context *)mc->reserved;
    uint64_t fpbuf[66] __attribute__((aligned(16)));
    fp_save(fpbuf);
    fc->magic = FPSIMD_MAGIC; fc->size = sizeof *fc;
    memcpy(fc->vregs, fpbuf, 512);
    fc->fpsr = fpbuf[64]; fc->fpcr = fpbuf[65];
    fr->fp = f->regs[29]; fr->lr = f->regs[30];
    int r = copy_to_user((void *)sp, fr, sizeof *fr);
    kfree(fr);
    if (r) return -EFAULT;
    struct rt_sigframe *u = (struct rt_sigframe *)sp;
    f->sp = sp;
    f->regs[0] = sig;
    f->regs[1] = (uint64_t)&u->info;
    f->regs[2] = (uint64_t)&u->uc;
    f->regs[29] = (uint64_t)&u->fp;
    f->regs[30] = ka->restorer;
    f->pc = ka->handler;
    return 0;
}

int arch_sigreturn(struct trap_frame *f, uint64_t *mask) {
    struct rt_sigframe *fr = kmalloc(sizeof *fr);
    if (!fr) return -ENOMEM;
    if (copy_from_user(fr, (void *)f->sp, sizeof *fr)) { kfree(fr); return -EFAULT; }
    struct sigcontext *mc = &fr->uc.mc;
    for (int i = 0; i < 31; i++) f->regs[i] = mc->regs[i];
    f->sp = mc->sp; f->pc = mc->pc;
    f->pstate = mc->pstate & 0xf0000000;                 /* NZCV only, EL0t */
    struct fpsimd_context *fc = (struct fpsimd_context *)mc->reserved;
    if (fc->magic == FPSIMD_MAGIC) {
        uint64_t fpbuf[66] __attribute__((aligned(16)));
        memcpy(fpbuf, fc->vregs, 512);
        fpbuf[64] = fc->fpsr; fpbuf[65] = fc->fpcr;
        fp_restore(fpbuf);
    }
    *mask = fr->uc.sigmask;
    kfree(fr);
    return 0;
}
