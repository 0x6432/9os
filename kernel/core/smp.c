/*
 * SMP support: per-CPU structures, the big kernel lock, AP bring-up via the Limine MP
 * request, IPIs and TLB shootdown.
 *
 * Locking model (M14): a Linux-2.0-style big kernel lock. Every trap/syscall entry takes the
 * BKL (recursively, depth tracked per thread) and drops it on return to user mode, and idle
 * CPUs drop it while halted. User code therefore runs truly in parallel, while kernel code is
 * serialised; IPI handlers run without the lock. CPUs spinning for the BKL keep servicing TLB
 * shootdown requests so a lock holder waiting for acknowledgements cannot deadlock.
 */
#include <kernel/sched.h>
#include <kernel/cpu.h>
#include <kernel/boot.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/time.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>

extern volatile bool panicking;
struct cpu cpus[MAX_CPUS];
int ncpus = 1;

/* ticket lock: FIFO fairness so a CPU that keeps re-entering the kernel cannot starve others */
static volatile uint32_t bkl_next, bkl_serving;
volatile int bkl_owner = -1;


static void bkl_lock(void) {
    uint32_t me = __atomic_fetch_add(&bkl_next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&bkl_serving, __ATOMIC_ACQUIRE) != me) {
        if (this_cpu()->ipi_pending || panicking) ipi_handle();
        arch_cpu_relax();
    }
    bkl_owner = this_cpu()->id;
}
static void bkl_unlock(void) {
    if (bkl_owner != this_cpu()->id) panic("bkl_unlock: owner cpu%d, unlocking on cpu%d (%s)", bkl_owner, this_cpu()->id, current ? current->name : "?");
    bkl_owner = -1; __atomic_store_n(&bkl_serving, bkl_serving + 1, __ATOMIC_RELEASE); }

/* The lock is taken with interrupts off: an interrupt arriving while we spin must not see
 * a non-zero depth and assume the lock is already held. */
void bkl_enter(void) {
    struct thread *t = current;
    if (!t) return;
    if (t->bkl_depth) { t->bkl_depth++; return; }
    uint64_t f = arch_irq_save();
    bkl_lock();
    t->bkl_depth = 1;
    arch_irq_restore(f);
}

void bkl_exit(void) {
    struct thread *t = current;
    if (!t) return;
    if (t->bkl_depth <= 0) panic("bkl_exit: depth %d in %s", t->bkl_depth, t->name);
    if (t->bkl_depth > 1) { t->bkl_depth--; return; }
    uint64_t f = arch_irq_save();
    t->bkl_depth = 0;
    bkl_unlock();
    arch_irq_restore(f);
}

void bkl_release_idle(void) {
    if (current->bkl_depth != 1) panic("bkl_release_idle: depth %d", current->bkl_depth);
    current->bkl_depth = 0;
    bkl_unlock();
}

void bkl_acquire_idle(void) {
    bkl_lock();
    current->bkl_depth = 1;
}

bool bkl_held(void) { return current && current->bkl_depth > 0; }

void smp_stop_others(void) {
    for (int i = 0; i < ncpus; i++) if (&cpus[i] != this_cpu() && cpus[i].online) arch_send_ipi(&cpus[i]);
}

void ipi_handle(void) {
    struct cpu *c = this_cpu();
    if (panicking) arch_halt_forever();
    uint32_t p = __atomic_load_n(&c->ipi_pending, __ATOMIC_ACQUIRE);
    if (p & IPI_TLB_FLUSH) {
        arch_tlb_flush_local();
        __atomic_and_fetch(&c->ipi_pending, ~IPI_TLB_FLUSH, __ATOMIC_RELEASE);
    }
    if (p & IPI_RESCHED) __atomic_and_fetch(&c->ipi_pending, ~IPI_RESCHED, __ATOMIC_RELEASE);
    /* a resched IPI only needs to wake the CPU from its idle halt */
}

void smp_send_resched(struct cpu *c) {
    __atomic_or_fetch(&c->ipi_pending, IPI_RESCHED, __ATOMIC_RELEASE);
    arch_send_ipi(c);
}

/* Default remote TLB invalidation: IPI every CPU in the mask and wait for it to flush. */
[[gnu::weak]] void arch_tlb_remote(uint64_t mask, vaddr_t va) {
    for (int i = 0; i < ncpus; i++)
        if (mask & (1ULL << i)) {
            __atomic_or_fetch(&cpus[i].ipi_pending, IPI_TLB_FLUSH, __ATOMIC_RELEASE);
            arch_send_ipi(&cpus[i]);
        }
    for (int i = 0; i < ncpus; i++)
        if (mask & (1ULL << i))
            while (__atomic_load_n(&cpus[i].ipi_pending, __ATOMIC_ACQUIRE) & IPI_TLB_FLUSH) arch_cpu_relax();
}

void tlb_shootdown(paddr_t root, vaddr_t va) {
    if (ncpus < 2) return;
    struct cpu *self = this_cpu();
    uint64_t mask = 0;
    for (int i = 0; i < ncpus; i++)
        if (&cpus[i] != self && cpus[i].online && cpus[i].active_root == root) mask |= 1ULL << i;
    if (mask) arch_tlb_remote(mask, va);
}

/* ---------------------------------------------------------------- bring-up */
static int cmdline_has(const char *w) {
    const char *s = boot_cmdline();
    size_t l = strlen(w);
    for (; *s; s++)
        if (!strncmp(s, w, l) && (s[l] == 0 || s[l] == ' ' || s[l] == '=')) return 1;
    return 0;
}

__noreturn void smp_ap_main(struct cpu *c) {
    sched_start_ap(c);
}

void smp_init(void) {
    struct limine_mp_response *mp = boot_mp();
    if (!mp || mp->cpu_count <= 1) { pr_info("smp: 1 CPU\n"); return; }
    if (cmdline_has("nosmp")) { pr_info("smp: disabled by 'nosmp' (%lu CPUs present)\n", mp->cpu_count); return; }
    int want = MAX_CPUS;
    const char *m = boot_cmdline();
    for (; *m; m++) if (!strncmp(m, "maxcpus=", 8)) { want = 0; for (m += 8; *m >= '0' && *m <= '9'; m++) want = want * 10 + *m - '0'; break; }
    if (want < 1) want = 1;
    uint64_t t0 = time_ns();
    int started = 0;
    for (uint64_t i = 0; i < mp->cpu_count; i++) {
        struct limine_mp_info *info = mp->cpus[i];
#if defined(__x86_64__)
        uint64_t hw = info->lapic_id, bsp = mp->bsp_lapic_id;
#elif defined(__aarch64__)
        uint64_t hw = info->mpidr, bsp = mp->bsp_mpidr;
#else
        uint64_t hw = info->hartid, bsp = mp->bsp_hartid;
#endif
        if (hw == bsp) { cpus[0].hwid = hw; continue; }
        if (ncpus >= MAX_CPUS || ncpus >= want) break;
        struct cpu *c = &cpus[ncpus];
        c->self = c;
        c->id = ncpus;
        c->hwid = hw;
        sched_init_ap(c);
        paddr_t stk = pmm_alloc_pages(2);
        if (!stk) break;
        c->kernel_sp = (uint64_t)PHYS_TO_VIRT(stk) + 4 * PAGE_SIZE;   /* AP boot stack */
        ncpus++;            /* visible before it comes online so IPIs/shootdowns can target it */
        arch_ap_boot(c, info);
        uint64_t deadline = time_ns() + 1000000000ULL;
        while (!__atomic_load_n(&c->online, __ATOMIC_ACQUIRE) && time_ns() < deadline) {
            /* the AP needs the BKL briefly to start its idle thread */
            bkl_release_idle();
            udelay(20);
            bkl_acquire_idle();
        }
        if (!c->online) { pr_warn("smp: cpu %d (hw %lx) did not come up (stage %lx)\n", c->id, hw, c->arch_data[1]); ncpus--; break; }
        started++;
    }
    pr_info("smp: %d CPUs online (%d APs started in %lu us)\n", ncpus, started, (time_ns() - t0) / 1000);
}
