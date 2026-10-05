#pragma once
struct thread;
struct cpu;
extern struct cpu cpus[];
extern _Thread_local int test_cpu;
static inline struct thread *arch_current(void) { return 0; }
static inline void arch_set_current(struct thread *t) {}
#define ARCH_HAS_THIS_CPU 1
static inline struct cpu *this_cpu(void) { return &cpus[test_cpu]; }