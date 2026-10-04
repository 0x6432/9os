#pragma once
/* aarch64: TPIDR_EL1 holds the running thread. */
struct thread;
static inline struct thread *arch_current(void) {
    struct thread *t;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(t));
    return t;
}
static inline void arch_set_current(struct thread *t) { __asm__ volatile("msr tpidr_el1, %0" :: "r"(t) : "memory"); }
