/* x86_64 user-mode support: syscall MSRs, per-CPU block, user thread setup. */
#include <kernel/sched.h>
#include <kernel/process.h>
#include <kernel/mm.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/trapframe.h>


void syscall_entry(void);
void x86_user_return(void);
extern uint8_t fpu_initial_state[512];

/* per-CPU block: GS base while in kernel mode (see arch/percpu.h) */
void x86_set_cpu_base(struct cpu *c) {
    c->self = c;
    wrmsr(0xC0000101, (uint64_t)c);         /* GS base (kernel) */
}

void syscall_init(void) {
    wrmsr(0xC0000102, 0);                   /* kernel GS base (= user GS while in kernel) */
    wrmsr(0xC0000080, rdmsr(0xC0000080) | 1);   /* EFER.SCE */
    wrmsr(0xC0000081, ((uint64_t)0x10 << 48) | ((uint64_t)KERNEL_CS << 32));
    wrmsr(0xC0000082, (uint64_t)syscall_entry);
    wrmsr(0xC0000084, 0x47700);             /* clear TF, IF, DF, AC, NT on entry */
}

void arch_set_kernel_stack(uint64_t top) {
    struct cpu *c = this_cpu();
    c->kernel_sp = top;
    tss_set_kernel_stack(c->id, top);
}

struct trap_frame *thread_user_frame(struct thread *t) {
    return (struct trap_frame *)((uint8_t *)t->kstack + KSTACK_SIZE) - 1;
}

/* Prepare t so that its first switch-in returns to user mode through its user frame. */
void arch_thread_init_user(struct thread *t) {
    uint64_t *sp = (uint64_t *)thread_user_frame(t);
    *--sp = (uint64_t)x86_user_return;
    for (int i = 0; i < 6; i++) *--sp = 0;
    t->arch.rsp = (uint64_t)sp;
    memcpy(t->arch.fpu, fpu_initial_state, 512);
}

void arch_thread_copy_fpu(struct thread *dst, struct thread *src) {
    if (src == current) __asm__ volatile("fxsave64 (%0)" :: "r"(src->arch.fpu) : "memory");
    memcpy(dst->arch.fpu, src->arch.fpu, 512);
    dst->arch.fs_base = src == current ? rdmsr(0xC0000100) : src->arch.fs_base;
}

void arch_reset_fpu(struct thread *t) {
    memcpy(t->arch.fpu, fpu_initial_state, 512);
    __asm__ volatile("fxrstor64 (%0)" :: "r"(t->arch.fpu) : "memory");
}

void arch_set_tls(struct thread *t, uint64_t v) {
    t->arch.fs_base = v;
    if (t == current) wrmsr(0xC0000100, v);
}

int64_t sys_arch_prctl(int code, uint64_t addr) {
    switch (code) {
    case 0x1002: arch_set_tls(current, addr); return 0;                   /* ARCH_SET_FS */
    case 0x1003: return copy_to_user((void *)addr, &current->arch.fs_base, 8); /* ARCH_GET_FS */
    case 0x1001: wrmsr(0xC0000102, addr); return 0;                       /* ARCH_SET_GS */
    default: return -EINVAL;
    }
}

void arch_switch_mm(struct thread *prev, struct thread *next) {
    if (next->proc) vmm_switch(next->proc->mm->pt);
    else vmm_switch(kernel_pt);
}

void dump_frame(struct trap_frame *f);

/* #PF fast path, run before the BKL: demand-zero/COW faults only need the mm lock */
bool page_fault_fast(struct trap_frame *f) {
    uint64_t addr = read_cr2();
    if (addr >= USER_TOP || !current || !current->proc) return false;
    return mm_handle_fault(current->proc->mm, addr, f->error & 2, f->error & 16);
}

/* #PF: demand paging for user addresses */
bool page_fault_handler(struct trap_frame *f) {
    uint64_t addr = read_cr2();
    if (!current || !current->proc) return false;
    bool write = f->error & 2, exec = f->error & 16;
    if (addr < USER_TOP && mm_handle_fault(current->proc->mm, addr, write, exec)) return true;
    if (trap_from_user(f)) {
        pr_debug("segfault pid %d at %lx rip %lx\n", current->proc->pid, addr, f->rip);
        signal_force_info(current, SIGSEGV, (f->error & 1) ? SEGV_ACCERR : SEGV_MAPERR, addr);
        return true;
    }
    return false;
}

bool user_exception(struct trap_frame *f) {
    int sig, code = SI_KERNEL;
    uint64_t addr = f->rip;
    switch (f->vector) {
    case 0: sig = SIGFPE; code = FPE_INTDIV; break;
    case 16: case 19: sig = SIGFPE; code = 0; break;
    case 6: sig = SIGILL; code = ILL_ILLOPC; break;
    case 3: sig = SIGTRAP; break;                       /* int3: SI_KERNEL like Linux */
    case 1: {                                           /* #DB: single step or debug register hit */
        uint64_t dr6;
        __asm__ volatile("mov %%dr6, %0" : "=r"(dr6));
        __asm__ volatile("mov %0, %%dr6" :: "r"(0xffff0ff0ULL));
        current->arch.dr6 = dr6;
        sig = SIGTRAP; code = TRAP_TRACE;
        if (dr6 & 0xf) {
            int n = __builtin_ctzll(dr6 & 0xf);
            uint64_t rw = (current->arch.dr7 >> (16 + 4 * n)) & 3;
            code = TRAP_HWBKPT;
            addr = current->arch.dr[n];
            if (rw == 0) f->rflags |= 0x10000;          /* RF: an execute breakpoint does not refire */
        }
        if (dr6 & 0x4000) f->rflags &= ~0x100ULL;       /* BS */
        if (!(dr6 & 0xf)) f->rflags &= ~0x100ULL;
        break;
    }
    case 13: case 12: case 11: sig = SIGSEGV; addr = 0; break;
    case 17: sig = SIGBUS; code = BUS_ADRALN; break;
    default: return false;
    }
    signal_force_info(current, sig, code, addr);
    return true;
}
