#pragma once
/* Per-CPU state and SMP interfaces. */
#include <kernel/types.h>

#define MAX_CPUS 32

struct thread;

/* The first four fields have fixed offsets: x86_64 assembly reaches them through %gs. */
struct cpu {
    struct cpu *self;              /* 0 */
    uint64_t kernel_sp;            /* 8: x86 syscall entry stack */
    uint64_t user_sp;              /* 16: x86 syscall scratch */
    struct thread *cur;            /* 24: running thread (x86 'current') */
    int id;                        /* logical index, 0 = boot CPU */
    uint64_t hwid;                 /* LAPIC id / hart id / MPIDR */
    struct thread *idle;
    volatile bool resched;          /* need_resched flag */
    volatile bool online;
    bool tick_accounted;           /* sched_tick_fast already did this tick's accounting */
    volatile uint32_t ipi_pending; /* IPI_* bits */
    paddr_t active_root;           /* page table root loaded on this CPU */
    uint64_t ticks, idle_ticks, ctx_switches;
    struct thread *prev;           /* thread switched away from (cleared by the next thread) */
    uint64_t arch_data[4];         /* arch private (e.g. GIC cpu mask) */
};

#define IPI_TLB_FLUSH (1u << 0)
#define IPI_RESCHED   (1u << 1)

extern struct cpu cpus[MAX_CPUS];
extern int ncpus;                  /* CPUs brought online */

/* Big kernel lock: kernel code runs serialised; user code runs in parallel on all CPUs. */
void bkl_enter(void);              /* trap/syscall entry (recursive per thread) */
void bkl_exit(void);               /* trap/syscall exit */
void bkl_release_idle(void);       /* idle loop: drop the lock before halting */
void bkl_acquire_idle(void);
bool bkl_held(void);

void smp_init(void);               /* boot the application processors */
__noreturn void smp_ap_main(struct cpu *c);   /* common AP entry after arch setup (does not return) */
void ipi_handle(void);             /* run pending IPI work on this CPU (no BKL needed) */
void smp_send_resched(struct cpu *c);
/* invalidate stale user TLB entries for page table 'root' on all other CPUs using it */
void tlb_shootdown(paddr_t root, vaddr_t va);

/* provided by arch */
void arch_send_ipi(struct cpu *c);
int arch_cpu_hw_index(void);       /* used when the per-CPU pointer is not set up yet */
void arch_ap_boot(struct cpu *c, void *mp_info);   /* start AP; must call smp_ap_main */
void arch_tlb_flush_local(void);
