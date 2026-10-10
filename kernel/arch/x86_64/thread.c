#include <kernel/sched.h>
#include <kernel/string.h>
#include <arch/gdt.h>
#include <arch/cpu.h>
#include <arch/trapframe.h>

void x86_switch_stack(uint64_t *old_rsp, uint64_t new_rsp);
void x86_thread_trampoline(void);
extern uint8_t fpu_initial_state[512];

void arch_thread_init(struct thread *t, void (*entry)(void *), void *arg) {
    /* the top of every kernel stack is reserved for the user trap frame */
    uint64_t *sp = (uint64_t *)((uint8_t *)t->kstack + KSTACK_SIZE - sizeof(struct trap_frame));
    sp = (uint64_t *)((uint64_t)sp & ~15ULL);
    *--sp = 0;                               /* alignment / fake return */
    *--sp = (uint64_t)x86_thread_trampoline;
    *--sp = 0;                               /* rbp */
    *--sp = 0;                               /* rbx */
    *--sp = (uint64_t)entry;                 /* r12 */
    *--sp = (uint64_t)arg;                   /* r13 */
    *--sp = 0;                               /* r14 */
    *--sp = 0;                               /* r15 */
    t->arch.rsp = (uint64_t)sp;
    memcpy(t->arch.fpu, fpu_initial_state, 512);
}

void arch_switch_mm(struct thread *prev, struct thread *next);
void arch_set_kernel_stack(uint64_t top);

void arch_switch_to(struct thread *prev, struct thread *next) {
    __asm__ volatile("fxsave64 (%0)" :: "r"(prev->arch.fpu) : "memory");
    prev->arch.fs_base = rdmsr(0xC0000100);
    arch_switch_mm(prev, next);
    arch_set_kernel_stack((uint64_t)next->kstack + KSTACK_SIZE);
    wrmsr(0xC0000100, next->arch.fs_base);
    __asm__ volatile("fxrstor64 (%0)" :: "r"(next->arch.fpu) : "memory");
    if (prev->arch.dr7 || next->arch.dr7) {         /* debug registers follow the thread */
        if (next->arch.dr7) {
            __asm__ volatile("mov %0, %%dr7" :: "r"(0ULL));
            __asm__ volatile("mov %0, %%dr0" :: "r"(next->arch.dr[0]));
            __asm__ volatile("mov %0, %%dr1" :: "r"(next->arch.dr[1]));
            __asm__ volatile("mov %0, %%dr2" :: "r"(next->arch.dr[2]));
            __asm__ volatile("mov %0, %%dr3" :: "r"(next->arch.dr[3]));
        }
        __asm__ volatile("mov %0, %%dr7" :: "r"(next->arch.dr7));
    }
    x86_switch_stack(&prev->arch.rsp, next->arch.rsp);
}

/* ptrace detach / exec: drop the thread's hardware breakpoints */
void arch_hw_debug_reset(struct thread *t) {
    memset(t->arch.dr, 0, sizeof t->arch.dr);
    t->arch.dr6 = 0; t->arch.dr7 = 0;
    if (t == current) __asm__ volatile("mov %0, %%dr7" :: "r"(0ULL));
}
