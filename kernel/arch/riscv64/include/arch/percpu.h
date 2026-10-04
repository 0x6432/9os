#pragma once
/* riscv64: tp holds the running thread while in kernel mode (sscratch holds it in user mode). */
struct thread;
static inline struct thread *arch_current(void) {
    struct thread *t;
    __asm__ volatile("mv %0, tp" : "=r"(t));
    return t;
}
static inline void arch_set_current(struct thread *t) { __asm__ volatile("mv tp, %0" :: "r"(t) : "memory"); }
