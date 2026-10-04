/* riscv64 thread contexts, FPU state and TLS (tp register). */
#include <kernel/sched.h>
#include <kernel/process.h>
#include <kernel/vmm.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <arch/trapframe.h>
#include <arch/cpu.h>

void riscv_switch_stack(uint64_t *old_sp, uint64_t new_sp);
void riscv_thread_trampoline(void);
void riscv_user_return(void);
void fp_save(uint64_t *buf);
void fp_restore(const uint64_t *buf);

#define SWITCH_WORDS 14   /* ra, s0-s11, pad */

struct trap_frame *thread_user_frame(struct thread *t) {
    return (struct trap_frame *)((uint8_t *)t->kstack + KSTACK_SIZE) - 1;
}

void arch_thread_init(struct thread *t, void (*entry)(void *), void *arg) {
    uint64_t *sp = (uint64_t *)thread_user_frame(t) - SWITCH_WORDS;
    memset(sp, 0, SWITCH_WORDS * 8);
    sp[0] = (uint64_t)riscv_thread_trampoline;
    sp[2] = (uint64_t)entry;      /* s1 */
    sp[3] = (uint64_t)arg;        /* s2 */
    t->arch.sp = (uint64_t)sp;
    t->arch.ktop = (uint64_t)t->kstack + KSTACK_SIZE;
    memset(t->arch.fpu, 0, sizeof t->arch.fpu);
}

void arch_thread_init_user(struct thread *t) {
    uint64_t *sp = (uint64_t *)thread_user_frame(t) - SWITCH_WORDS;
    memset(sp, 0, SWITCH_WORDS * 8);
    sp[0] = (uint64_t)riscv_user_return;
    t->arch.sp = (uint64_t)sp;
    t->arch.ktop = (uint64_t)t->kstack + KSTACK_SIZE;
    memset(t->arch.fpu, 0, sizeof t->arch.fpu);
}

void arch_thread_copy_fpu(struct thread *dst, struct thread *src) {
    if (src == current) fp_save(src->arch.fpu);
    memcpy(dst->arch.fpu, src->arch.fpu, sizeof dst->arch.fpu);
}

void arch_reset_fpu(struct thread *t) {
    memset(t->arch.fpu, 0, sizeof t->arch.fpu);
    if (t == current) fp_restore(t->arch.fpu);
}

void arch_set_tls(struct thread *t, uint64_t v) { thread_user_frame(t)->regs[4] = v; }

void arch_switch_mm(struct thread *prev, struct thread *next) {
    if (next->proc) vmm_switch(next->proc->mm->pt);
    else vmm_switch(kernel_pt);
}

void arch_switch_to(struct thread *prev, struct thread *next) {
    fp_save(prev->arch.fpu);
    arch_switch_mm(prev, next);
    fp_restore(next->arch.fpu);
    riscv_switch_stack(&prev->arch.sp, next->arch.sp);
}
