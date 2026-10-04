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

struct percpu { uint64_t self, kernel_rsp, user_rsp; };
static struct percpu cpu0;

void syscall_entry(void);
void x86_user_return(void);
extern uint8_t fpu_initial_state[512];

void syscall_init(void) {
    cpu0.self = (uint64_t)&cpu0;
    wrmsr(0xC0000101, (uint64_t)&cpu0);     /* GS base (kernel) */
    wrmsr(0xC0000102, 0);                   /* kernel GS base (= user GS while in kernel) */
    wrmsr(0xC0000080, rdmsr(0xC0000080) | 1);   /* EFER.SCE */
    wrmsr(0xC0000081, ((uint64_t)0x10 << 48) | ((uint64_t)KERNEL_CS << 32));
    wrmsr(0xC0000082, (uint64_t)syscall_entry);
    wrmsr(0xC0000084, 0x47700);             /* clear TF, IF, DF, AC, NT on entry */
}

void arch_set_kernel_stack(uint64_t top) {
    cpu0.kernel_rsp = top;
    tss_set_kernel_stack(top);
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

/* #PF: demand paging for user addresses */
bool page_fault_handler(struct trap_frame *f) {
    uint64_t addr = read_cr2();
    if (addr >= USER_TOP || !current || !current->proc) return false;
    bool write = f->error & 2, exec = f->error & 16;
    if (mm_handle_fault(current->proc->mm, addr, write, exec)) return true;
    if (trap_from_user(f)) {
        pr_debug("segfault pid %d at %lx rip %lx\n", current->proc->pid, addr, f->rip);
        signal_force(current, SIGSEGV);
        return true;
    }
    return false;
}

bool user_exception(struct trap_frame *f) {
    int sig;
    switch (f->vector) {
    case 0: case 16: case 19: sig = SIGFPE; break;
    case 6: sig = SIGILL; break;
    case 3: case 1: sig = SIGTRAP; break;
    case 13: case 12: case 11: sig = SIGSEGV; break;
    case 17: sig = SIGBUS; break;
    default: return false;
    }
    signal_force(current, sig);
    return true;
}
