#pragma once
struct thread;
struct cpu;
extern struct cpu cpus[];
extern struct thread *test_current;
static inline struct thread *arch_current(void) { return test_current; }
static inline void arch_set_current(struct thread *t) { test_current = t; }
#define ARCH_HAS_THIS_CPU 1
static inline struct cpu *this_cpu(void) { return &cpus[0]; }