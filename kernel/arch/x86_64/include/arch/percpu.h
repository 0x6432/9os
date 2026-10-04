#pragma once
/* x86_64: %gs base points at this CPU's struct cpu while in kernel mode. */
struct thread;
struct cpu;
static inline struct thread *arch_current(void) {
    struct thread *t;
    __asm__ volatile("movq %%gs:24, %0" : "=r"(t));
    return t;
}
static inline void arch_set_current(struct thread *t) { __asm__ volatile("movq %0, %%gs:24" :: "r"(t) : "memory"); }
#define ARCH_HAS_THIS_CPU 1
static inline struct cpu *this_cpu(void) {
    struct cpu *c;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(c));
    return c;
}
