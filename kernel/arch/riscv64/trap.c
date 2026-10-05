/* riscv64 trap dispatch: interrupts, syscalls (ecall), page faults, exceptions. */
#include <kernel/printk.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <arch/trapframe.h>
#include <arch/cpu.h>

void syscall_dispatch(struct trap_frame *f);
void user_return_work(struct trap_frame *f);
void riscv_timer_irq(void);
void riscv_timer_rearm(void);

static const char *exc_names[16] = {
    "instruction misaligned", "instruction access fault", "illegal instruction", "breakpoint",
    "load misaligned", "load access fault", "store misaligned", "store access fault",
    "ecall from U", "ecall from S", "?", "?", "instruction page fault", "load page fault", "?",
    "store page fault",
};

void dump_frame(struct trap_frame *f) {
    printk("  sepc=%016lx sstatus=%016lx scause=%lx stval=%016lx\n", f->sepc, f->sstatus, f->scause, f->stval);
    static const char *n[32] = { "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1",
        "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11",
        "t3", "t4", "t5", "t6" };
    for (int i = 1; i < 32; i++) printk("  %-4s=%016lx%s", n[i], f->regs[i], i % 4 == 3 ? "\n" : "");
    printk("\n");
    if (!trap_from_user(f)) {
        uint64_t *fp = (uint64_t *)f->regs[8];
        printk("  backtrace: %lx", f->regs[1]);
        for (int i = 0; i < 12 && fp && (uint64_t)fp >= 0xffff800000000000ULL; i++) {
            printk(" %lx", fp[-1]);
            fp = (uint64_t *)fp[-2];
        }
        printk("\n");
    }
}

static bool handle_page_fault(struct trap_frame *f) {
    uint64_t addr = f->stval, c = f->scause;
    if (addr >= USER_TOP || !current || !current->proc) return false;
    if (mm_handle_fault(current->proc->mm, addr, c == 15, c == 12)) return true;
    if (trap_from_user(f)) {
        pr_debug("segfault pid %d at %lx pc %lx\n", current->proc->pid, addr, f->sepc);
        signal_force(current, SIGSEGV);
        return true;
    }
    return false;
}

void trap_dispatch(struct trap_frame *f) {
    uint64_t c = f->scause;
    if (c == (1ULL << 63 | 1)) {      /* supervisor software interrupt = IPI, no BKL */
        csr_clear(sip, SIE_SSIE);
        ipi_handle();
        return;
    }
    if (c == (1ULL << 63 | 5)) {      /* timer: secondary harts usually need no lock */
        riscv_timer_rearm();
        if (sched_tick_fast(trap_from_user(f))) return;
    }
    if (c == 8) {
        f->sepc += 4;
        f->orig_a0 = f->regs[10];
        arch_irq_enable();
        syscall_dispatch(f);        /* takes the BKL; also runs user_return_work */
        return;
    }
    if ((c == 12 || c == 13 || c == 15) && f->stval < USER_TOP && current && current->proc &&
        mm_handle_fault(current->proc->mm, f->stval, c == 15, c == 12)) {   /* lock-free fault fast path */
        if (trap_from_user(f)) user_return_work(f);
        return;
    }
    bkl_enter();
    if ((int64_t)c < 0) {
        switch (c & 0xff) {
        case 5: riscv_timer_irq(); break;
        default: printk("spurious interrupt %lu\n", c & 0xff);
        }
    } else {
        if ((c == 12 || c == 13 || c == 15) && handle_page_fault(f)) goto out;
        if (trap_from_user(f)) {
            int sig = SIGSEGV;
            if (c == 2) sig = SIGILL;
            else if (c == 3) sig = SIGTRAP;
            else if (c == 0 || c == 4 || c == 6) sig = SIGBUS;
            signal_force(current, sig);
            goto out;
        }
        printk("\nexception %lu (%s)\n", c, c < 16 ? exc_names[c] : "?");
        dump_frame(f);
        panic("unhandled exception in kernel mode");
    }
out:
    user_return_work(f);
    bkl_exit();
}
