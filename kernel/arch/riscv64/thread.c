/* riscv64 thread contexts, FPU state and TLS (tp register). */
#include <kernel/sched.h>
#include <kernel/process.h>
#include <kernel/vmm.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <arch/trapframe.h>
#include <arch/cpu.h>

void riscv_switch_stack(uint64_t *old_sp, uint64_t new_sp);
void riscv_thread_trampoline(void);
void riscv_user_return(void);
void fp_save(uint64_t *buf);
void fp_restore(const uint64_t *buf);

#define SWITCH_WORDS 14   /* ra, s0-s11, pad */

struct trap_frame *thread_user_frame(struct thread *t) {
    return (struct trap_frame *)((uint8_t *)t->kstack + KSTACK_SIZE) - 1;
}

void arch_thread_init(struct thread *t, void (*entry)(void *), void *arg) {
    uint64_t *sp = (uint64_t *)thread_user_frame(t) - SWITCH_WORDS;
    memset(sp, 0, SWITCH_WORDS * 8);
    sp[0] = (uint64_t)riscv_thread_trampoline;
    sp[2] = (uint64_t)entry;      /* s1 */
    sp[3] = (uint64_t)arg;        /* s2 */
    t->arch.sp = (uint64_t)sp;
    t->arch.ktop = (uint64_t)t->kstack + KSTACK_SIZE;
    memset(t->arch.fpu, 0, sizeof t->arch.fpu);
}

void arch_thread_init_user(struct thread *t) {
    uint64_t *sp = (uint64_t *)thread_user_frame(t) - SWITCH_WORDS;
    memset(sp, 0, SWITCH_WORDS * 8);
    sp[0] = (uint64_t)riscv_user_return;
    t->arch.sp = (uint64_t)sp;
    t->arch.ktop = (uint64_t)t->kstack + KSTACK_SIZE;
    memset(t->arch.fpu, 0, sizeof t->arch.fpu);
}

void arch_thread_copy_fpu(struct thread *dst, struct thread *src) {
    if (src == current) fp_save(src->arch.fpu);
    memcpy(dst->arch.fpu, src->arch.fpu, sizeof dst->arch.fpu);
}

void arch_reset_fpu(struct thread *t) {
    memset(t->arch.fpu, 0, sizeof t->arch.fpu);
    if (t == current) fp_restore(t->arch.fpu);
}

void arch_set_tls(struct thread *t, uint64_t v) { thread_user_frame(t)->regs[4] = v; }

void arch_switch_mm(struct thread *prev, struct thread *next) {
    if (next->proc) vmm_switch(next->proc->mm->pt);
    else vmm_switch(kernel_pt);
}

/*
 * Hardware breakpoints / watchpoints through the SBI Debug Triggers extension (DBTR, OpenSBI
 * >= 1.5 on Sdtrig harts): triggers are per hart, so a thread's triggers (mcontrol6, or legacy
 * mcontrol, U-mode only, action = breakpoint exception) are installed when it is switched in and
 * removed when it is switched out. Each hart registers a one-page shared memory area first.
 */
#include <kernel/cpu.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/printk.h>
#define SBI_EXT_DBTR 0x44425452
static int dbtr_total = -1;
static uint64_t dbtr_type;
static uint64_t *dbtr_shmem[MAX_CPUS];
static uint64_t dbtr_mask[MAX_CPUS];          /* installed trigger indexes on each hart */
static void dbtr_probe(void) {
    if (dbtr_total >= 0) return;
    dbtr_total = 0;
    if (!sbi_call(0x10, 3, SBI_EXT_DBTR, 0, 0).value) return;
    for (uint64_t ty = 6; ty >= 2; ty -= 4) {
        struct sbiret r = sbi_call(SBI_EXT_DBTR, 0, (long)(ty << 60), 0, 0);
        if (!r.error && r.value > 0) { dbtr_total = (int)MIN(r.value, 16L); dbtr_type = ty; break; }
    }
    if (!dbtr_total) return;
    for (int c = 0; c < ncpus; c++) {           /* per-hart shared memory, registered on first use */
        paddr_t pa = pmm_alloc_zeroed(0);
        if (!pa) { dbtr_total = 0; return; }
        dbtr_shmem[c] = PHYS_TO_VIRT(pa);
    }
    pr_info("riscv: %d SBI debug triggers (type %lu) for ptrace\n", dbtr_total, dbtr_type);
}
int arch_num_brps(void) { dbtr_probe(); return dbtr_total / 2; }
int arch_num_wrps(void) { dbtr_probe(); return dbtr_total - dbtr_total / 2; }
static bool dbtr_registered[MAX_CPUS];
static uint64_t *dbtr_area(void) {          /* runs in arch_switch_to: no allocation here */
    int c = this_cpu()->id;
    if (!dbtr_shmem[c]) return nullptr;
    if (!dbtr_registered[c]) {
        if (sbi_call(SBI_EXT_DBTR, 1, (long)VIRT_TO_PHYS(dbtr_shmem[c]), 0, 0).error) return nullptr;
        dbtr_registered[c] = true;
    }
    return dbtr_shmem[c];
}
static void dbtr_unload(void) {
    int c = this_cpu()->id;
    if (dbtr_mask[c]) sbi_call(SBI_EXT_DBTR, 5, 0, (long)dbtr_mask[c], 0);
    dbtr_mask[c] = 0;
}
static void dbtr_load(struct thread *t) {
    uint64_t *m = dbtr_area();
    if (!m) return;
    int n = 0, nb = arch_num_brps(), nw = arch_num_wrps();
    for (int i = 0; i < nb + nw; i++) {
        bool w = i >= nb;
        int k = w ? i - nb : i;
        uint32_t c = w ? t->arch.wcr[k] : t->arch.bcr[k];
        if (!(c & 1)) continue;
        uint64_t acc = w ? ((c >> 3) & 3) : 4;      /* load (bit 0) / store (bit 1) / execute (bit 2) */
        m[4 * n + 0] = 0;
        m[4 * n + 1] = (dbtr_type << 60) | (1 << 3) | acc;     /* U-mode, match equal, action 0 */
        m[4 * n + 2] = w ? t->arch.wvr[k] : t->arch.bvr[k];
        m[4 * n + 3] = 0;
        n++;
    }
    if (!n) return;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (sbi_call(SBI_EXT_DBTR, 3, n, 0, 0).error) return;
    int c = this_cpu()->id;
    for (int i = 0; i < n; i++) if (m[4 * i] < 64) dbtr_mask[c] |= 1ULL << m[4 * i];
}
void arch_hw_debug_reset(struct thread *t) {
    bool was = t->arch.hwdbg;
    memset(t->arch.bvr, 0, sizeof t->arch.bvr); memset(t->arch.wvr, 0, sizeof t->arch.wvr);
    memset(t->arch.bcr, 0, sizeof t->arch.bcr); memset(t->arch.wcr, 0, sizeof t->arch.wcr);
    t->arch.hwdbg = false;
    if (was && t == current) { uint64_t f = arch_irq_save(); dbtr_unload(); arch_irq_restore(f); }
}

void arch_switch_to(struct thread *prev, struct thread *next) {
    if (prev->arch.hwdbg || dbtr_mask[this_cpu()->id]) dbtr_unload();
    if (next->arch.hwdbg) dbtr_load(next);
    fp_save(prev->arch.fpu);
    arch_switch_mm(prev, next);
    fp_restore(next->arch.fpu);
    riscv_switch_stack(&prev->arch.sp, next->arch.sp);
}

