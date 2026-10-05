#pragma once
struct thread;
struct cpu;
extern struct cpu cpus[];
extern _Thread_local struct thread *test_current;
extern _Thread_local int sched_test_cpu;
static inline struct thread *arch_current(void) { return test_current; }
static inline void arch_set_current(struct thread *t) { test_current = t; }
#define ARCH_HAS_THIS_CPU 1
static inline struct cpu *this_cpu(void) { return &cpus[sched_test_cpu]; }