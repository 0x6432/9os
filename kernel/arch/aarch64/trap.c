/* aarch64 trap dispatch. */
#include <kernel/uaccess.h>
#include <kernel/printk.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <arch/trapframe.h>
#include <arch/cpu.h>

void syscall_dispatch(struct trap_frame *f);
void user_return_work(struct trap_frame *f);
void a64_irq(struct trap_frame *f);

void dump_frame(struct trap_frame *f) {
    printk("  pc=%016lx pstate=%08lx esr=%08lx far=%016lx sp_el0=%016lx\n", f->pc, f->pstate, f->esr, f->far, f->sp);
    for (int i = 0; i < 31; i++) printk("  x%-2d=%016lx%s", i, f->regs[i], i % 4 == 3 ? "\n" : "");
    printk("\n");
    if (!trap_from_user(f)) {
        uint64_t *fp = (uint64_t *)f->regs[29];
        printk("  backtrace: %lx", f->regs[30]);
        for (int i = 0; i < 12 && fp && (uint64_t)fp >= 0xffff000000000000ULL; i++) {
            printk(" %lx", fp[1]);
            fp = (uint64_t *)fp[0];
        }
        printk("\n");
    }
}

static bool handle_abort(struct trap_frame *f, bool exec) {
    uint64_t addr = f->far;
    bool write = !exec && (f->esr & (1 << 6));
    if (addr >= USER_TOP || !current || !current->proc) return false;
    if (mm_handle_fault(current->proc->mm, addr, write, exec)) return true;
    if (trap_from_user(f)) {
        pr_debug("segfault pid %d at %lx pc %lx\n", current->proc->pid, addr, f->pc);
        signal_force_info(current, SIGSEGV, SEGV_MAPERR, addr);
        return true;
    }
    return false;
}

void trap_dispatch(struct trap_frame *f, int kind) {
    if (kind == 1 || kind == 3) {
        a64_irq(f);                 /* takes the BKL itself (IPIs run without it) */
        return;
    }
    if (kind == 0 || kind == 2) {
        uint32_t ec = f->esr >> 26;
        if (ec == 0x15 && kind == 2) {
            f->orig_x0 = f->regs[0];
            arch_irq_enable();
            syscall_dispatch(f);
            return;
        }
        if ((ec == 0x25 || ec == 0x21) && kernel_fault_check(f, f->far, ec == 0x21)) return;   /* PXN/PAN triage */
        if (ec == 0x24 || ec == 0x25 || ec == 0x20 || ec == 0x21) {     /* lock-free fault fast path */
            bool exec = ec == 0x20 || ec == 0x21;
            if (f->far < USER_TOP && current && current->proc &&
                mm_handle_fault(current->proc->mm, f->far, !exec && (f->esr & (1 << 6)), exec)) {
                if (kind == 2) user_return_work(f);
                return;
            }
        }
        if ((ec == 0x25 || ec == 0x21) && kernel_fault_fixup(f)) return;   /* exception table */
        bkl_enter();
        if ((ec == 0x24 || ec == 0x25) && handle_abort(f, false)) goto out;
        if ((ec == 0x20 || ec == 0x21) && handle_abort(f, true)) goto out;
        if (kind == 2) {
            int sig = SIGILL, code = ILL_ILLOPC;
            uint64_t addr = f->pc;
            if (ec == 0x24 || ec == 0x20) { sig = SIGSEGV; code = SEGV_ACCERR; addr = f->far; }
            else if (ec == 0x22 || ec == 0x26) { sig = SIGBUS; code = BUS_ADRALN; addr = f->far; }
            else if (ec == 0x3c) { sig = SIGTRAP; code = TRAP_BRKPT; }
            else if (ec == 0x32) {                          /* software step (ptrace) */
                sig = SIGTRAP; code = TRAP_TRACE;
                f->pstate &= ~(1ULL << 21);                 /* SPSR.SS */
            }
            else if (ec == 0x30) { sig = SIGTRAP; code = TRAP_HWBKPT; }                  /* hw breakpoint */
            else if (ec == 0x34) { sig = SIGTRAP; code = TRAP_HWBKPT; addr = f->far; }   /* watchpoint */
            else if (ec == 0x2c) { sig = SIGFPE; code = 0; }
            signal_force_info(current, sig, code, addr);
            goto out;
        }
        printk("\nexception: EC %#x\n", ec);
        dump_frame(f);
        panic("unhandled exception in kernel mode");
    } else {
        printk("\nunexpected exception vector\n");
        dump_frame(f);
        panic("bad vector");
    }
out:
    user_return_work(f);
    bkl_exit();
}
