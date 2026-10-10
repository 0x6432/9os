/* aarch64 thread contexts, FP/SIMD state and TLS (TPIDR_EL0). */
#include <kernel/sched.h>
#include <kernel/process.h>
#include <kernel/vmm.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <arch/trapframe.h>
#include <arch/cpu.h>

void a64_switch_stack(uint64_t *old_sp, uint64_t new_sp);
void a64_thread_trampoline(void);
void a64_user_return(void);
void fp_save(uint64_t *buf);
void fp_restore(const uint64_t *buf);

#define SWITCH_WORDS 12   /* x19-x30 */

struct trap_frame *thread_user_frame(struct thread *t) {
    return (struct trap_frame *)((uint8_t *)t->kstack + KSTACK_SIZE) - 1;
}

void arch_thread_init(struct thread *t, void (*entry)(void *), void *arg) {
    uint64_t *sp = (uint64_t *)thread_user_frame(t) - SWITCH_WORDS;
    memset(sp, 0, SWITCH_WORDS * 8);
    sp[0] = (uint64_t)entry;                      /* x19 */
    sp[1] = (uint64_t)arg;                        /* x20 */
    sp[11] = (uint64_t)a64_thread_trampoline;     /* x30 */
    t->arch.sp = (uint64_t)sp;
    t->arch.tpidr = 0;
    memset(t->arch.fpu, 0, sizeof t->arch.fpu);
}

void arch_thread_init_user(struct thread *t) {
    uint64_t *sp = (uint64_t *)thread_user_frame(t) - SWITCH_WORDS;
    memset(sp, 0, SWITCH_WORDS * 8);
    sp[11] = (uint64_t)a64_user_return;
    t->arch.sp = (uint64_t)sp;
    memset(t->arch.fpu, 0, sizeof t->arch.fpu);
}

void arch_thread_copy_fpu(struct thread *dst, struct thread *src) {
    if (src == current) { fp_save(src->arch.fpu); src->arch.tpidr = sysreg_read(tpidr_el0); }
    memcpy(dst->arch.fpu, src->arch.fpu, sizeof dst->arch.fpu);
    dst->arch.tpidr = src->arch.tpidr;
}

void arch_reset_fpu(struct thread *t) {
    memset(t->arch.fpu, 0, sizeof t->arch.fpu);
    if (t == current) fp_restore(t->arch.fpu);
}

void arch_set_tls(struct thread *t, uint64_t v) {
    t->arch.tpidr = v;
    if (t == current) sysreg_write(tpidr_el0, v);
}

void arch_switch_mm(struct thread *prev, struct thread *next) {
    if (next->proc) vmm_switch(next->proc->mm->pt);
    else vmm_switch(kernel_pt);
}

void arch_switch_to(struct thread *prev, struct thread *next) {
    fp_save(prev->arch.fpu);
    prev->arch.tpidr = sysreg_read(tpidr_el0);
    arch_switch_mm(prev, next);
    sysreg_write(tpidr_el0, next->arch.tpidr);
    fp_restore(next->arch.fpu);
    {   /* ptrace single step: MDSCR_EL1.SS follows the thread */
        uint64_t m = sysreg_read(mdscr_el1);
        if ((m & 1) != (uint64_t)next->pt_step) {
            sysreg_write(mdscr_el1, next->pt_step ? m | 1 : m & ~1ULL);
            __asm__ volatile("isb" ::: "memory");
        }
    }
    a64_switch_stack(&prev->arch.sp, next->arch.sp);
}
