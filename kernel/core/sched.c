/* Placeholder until M6 (round-robin scheduler). */
#include <kernel/sched.h>
#include <kernel/arch.h>
struct thread *current;
void sched_yield(void) { arch_cpu_relax(); }
