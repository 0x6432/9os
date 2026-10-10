/* aarch64 thread contexts, FP/SIMD state and TLS (TPIDR_EL0). */
#include <kernel/sched.h>
#include <kernel/process.h>
#include <kernel/vmm.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <arch/trapframe.h>
#include <arch/cpu.h>

void a64_switch_stack(uint64_t *old_sp, uint64_t new_sp);
void a64_thread_trampoline(void);
void a64_user_return(void);
void fp_save(uint64_t *buf);
void fp_restore(const uint64_t *buf);

#define SWITCH_WORDS 12   /* x19-x30 */

struct trap_frame *thread_user_frame(struct thread *t) {
    return (struct trap_frame *)((uint8_t *)t->kstack + KSTACK_SIZE) - 1;
}

void arch_thread_init(struct thread *t, void (*entry)(void *), void *arg) {
    uint64_t *sp = (uint64_t *)thread_user_frame(t) - SWITCH_WORDS;
    memset(sp, 0, SWITCH_WORDS * 8);
    sp[0] = (uint64_t)entry;                      /* x19 */
    sp[1] = (uint64_t)arg;                        /* x20 */
    sp[11] = (uint64_t)a64_thread_trampoline;     /* x30 */
    t->arch.sp = (uint64_t)sp;
    t->arch.tpidr = 0;
    memset(t->arch.fpu, 0, sizeof t->arch.fpu);
}

void arch_thread_init_user(struct thread *t) {
    uint64_t *sp = (uint64_t *)thread_user_frame(t) - SWITCH_WORDS;
    memset(sp, 0, SWITCH_WORDS * 8);
    sp[11] = (uint64_t)a64_user_return;
    t->arch.sp = (uint64_t)sp;
    memset(t->arch.fpu, 0, sizeof t->arch.fpu);
}

void arch_thread_copy_fpu(struct thread *dst, struct thread *src) {
    if (src == current) { fp_save(src->arch.fpu); src->arch.tpidr = sysreg_read(tpidr_el0); }
    memcpy(dst->arch.fpu, src->arch.fpu, sizeof dst->arch.fpu);
    dst->arch.tpidr = src->arch.tpidr;
}

void arch_reset_fpu(struct thread *t) {
    memset(t->arch.fpu, 0, sizeof t->arch.fpu);
    if (t == current) fp_restore(t->arch.fpu);
}

void arch_set_tls(struct thread *t, uint64_t v) {
    t->arch.tpidr = v;
    if (t == current) sysreg_write(tpidr_el0, v);
}

void arch_switch_mm(struct thread *prev, struct thread *next) {
    if (next->proc) vmm_switch(next->proc->mm->pt);
    else vmm_switch(kernel_pt);
}


/* hardware breakpoints / watchpoints (ptrace NT_ARM_HW_BREAK/WATCH): per-thread register images
 * loaded on switch; MDSCR_EL1.MDE is set while a thread with armed slots runs */
static void dbg_b(int i, uint64_t v, uint64_t c) {
    switch (i) {
    case 0: sysreg_write(dbgbvr0_el1, v); sysreg_write(dbgbcr0_el1, c); break;
    case 1: sysreg_write(dbgbvr1_el1, v); sysreg_write(dbgbcr1_el1, c); break;
    case 2: sysreg_write(dbgbvr2_el1, v); sysreg_write(dbgbcr2_el1, c); break;
    case 3: sysreg_write(dbgbvr3_el1, v); sysreg_write(dbgbcr3_el1, c); break;
    case 4: sysreg_write(dbgbvr4_el1, v); sysreg_write(dbgbcr4_el1, c); break;
    case 5: sysreg_write(dbgbvr5_el1, v); sysreg_write(dbgbcr5_el1, c); break;
    case 6: sysreg_write(dbgbvr6_el1, v); sysreg_write(dbgbcr6_el1, c); break;
    case 7: sysreg_write(dbgbvr7_el1, v); sysreg_write(dbgbcr7_el1, c); break;
    case 8: sysreg_write(dbgbvr8_el1, v); sysreg_write(dbgbcr8_el1, c); break;
    case 9: sysreg_write(dbgbvr9_el1, v); sysreg_write(dbgbcr9_el1, c); break;
    case 10: sysreg_write(dbgbvr10_el1, v); sysreg_write(dbgbcr10_el1, c); break;
    case 11: sysreg_write(dbgbvr11_el1, v); sysreg_write(dbgbcr11_el1, c); break;
    case 12: sysreg_write(dbgbvr12_el1, v); sysreg_write(dbgbcr12_el1, c); break;
    case 13: sysreg_write(dbgbvr13_el1, v); sysreg_write(dbgbcr13_el1, c); break;
    case 14: sysreg_write(dbgbvr14_el1, v); sysreg_write(dbgbcr14_el1, c); break;
    case 15: sysreg_write(dbgbvr15_el1, v); sysreg_write(dbgbcr15_el1, c); break;
    }
}
static void dbg_w(int i, uint64_t v, uint64_t c) {
    switch (i) {
    case 0: sysreg_write(dbgwvr0_el1, v); sysreg_write(dbgwcr0_el1, c); break;
    case 1: sysreg_write(dbgwvr1_el1, v); sysreg_write(dbgwcr1_el1, c); break;
    case 2: sysreg_write(dbgwvr2_el1, v); sysreg_write(dbgwcr2_el1, c); break;
    case 3: sysreg_write(dbgwvr3_el1, v); sysreg_write(dbgwcr3_el1, c); break;
    case 4: sysreg_write(dbgwvr4_el1, v); sysreg_write(dbgwcr4_el1, c); break;
    case 5: sysreg_write(dbgwvr5_el1, v); sysreg_write(dbgwcr5_el1, c); break;
    case 6: sysreg_write(dbgwvr6_el1, v); sysreg_write(dbgwcr6_el1, c); break;
    case 7: sysreg_write(dbgwvr7_el1, v); sysreg_write(dbgwcr7_el1, c); break;
    case 8: sysreg_write(dbgwvr8_el1, v); sysreg_write(dbgwcr8_el1, c); break;
    case 9: sysreg_write(dbgwvr9_el1, v); sysreg_write(dbgwcr9_el1, c); break;
    case 10: sysreg_write(dbgwvr10_el1, v); sysreg_write(dbgwcr10_el1, c); break;
    case 11: sysreg_write(dbgwvr11_el1, v); sysreg_write(dbgwcr11_el1, c); break;
    case 12: sysreg_write(dbgwvr12_el1, v); sysreg_write(dbgwcr12_el1, c); break;
    case 13: sysreg_write(dbgwvr13_el1, v); sysreg_write(dbgwcr13_el1, c); break;
    case 14: sysreg_write(dbgwvr14_el1, v); sysreg_write(dbgwcr14_el1, c); break;
    case 15: sysreg_write(dbgwvr15_el1, v); sysreg_write(dbgwcr15_el1, c); break;
    }
}
static int nbrps = -1, nwrps;
static void dbg_count(void) {
    if (nbrps >= 0) return;
    uint64_t d = sysreg_read(id_aa64dfr0_el1);
    nbrps = (int)((d >> 12) & 0xf) + 1; nwrps = (int)((d >> 20) & 0xf) + 1;
    if (nbrps > 16) nbrps = 16;
    if (nwrps > 16) nwrps = 16;
}
int arch_num_brps(void) { dbg_count(); return nbrps; }
int arch_num_wrps(void) { dbg_count(); return nwrps; }
static void dbg_load(struct thread *t) {
    dbg_count();
    for (int i = 0; i < nbrps; i++) dbg_b(i, t ? t->arch.bvr[i] : 0, t ? t->arch.bcr[i] : 0);
    for (int i = 0; i < nwrps; i++) dbg_w(i, t ? t->arch.wvr[i] : 0, t ? t->arch.wcr[i] : 0);
}
/* every CPU at boot: the slots reset to UNKNOWN values */
void a64_debug_init(void) { dbg_load(nullptr); __asm__ volatile("isb" ::: "memory"); }
void arch_hw_debug_reset(struct thread *t) {
    memset(t->arch.bvr, 0, sizeof t->arch.bvr); memset(t->arch.wvr, 0, sizeof t->arch.wvr);
    memset(t->arch.bcr, 0, sizeof t->arch.bcr); memset(t->arch.wcr, 0, sizeof t->arch.wcr);
    bool was = t->arch.hwdbg;
    t->arch.hwdbg = false;
    if (t == current && was) { dbg_load(nullptr); sysreg_write(mdscr_el1, sysreg_read(mdscr_el1) & ~(1ULL << 15)); __asm__ volatile("isb" ::: "memory"); }
}

void arch_switch_to(struct thread *prev, struct thread *next) {
    fp_save(prev->arch.fpu);
    prev->arch.tpidr = sysreg_read(tpidr_el0);
    arch_switch_mm(prev, next);
    sysreg_write(tpidr_el0, next->arch.tpidr);
    fp_restore(next->arch.fpu);
    if (prev->arch.hwdbg || next->arch.hwdbg) dbg_load(next->arch.hwdbg ? next : nullptr);
    {   /* ptrace single step (SS) and hardware breakpoints (MDE) follow the thread */
        uint64_t m = sysreg_read(mdscr_el1);
        uint64_t want = (m & ~((1ULL << 15) | 1)) | (next->pt_step ? 1 : 0) | (next->arch.hwdbg ? 1ULL << 15 : 0);
        if (want != m || prev->arch.hwdbg || next->arch.hwdbg) {
            sysreg_write(mdscr_el1, want);
            __asm__ volatile("isb" ::: "memory");
        }
    }
    a64_switch_stack(&prev->arch.sp, next->arch.sp);
}
