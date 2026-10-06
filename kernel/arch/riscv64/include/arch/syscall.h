#pragma once
#include <arch/trapframe.h>
#include <arch/cpu.h>
#define SC_NR(f)    ((f)->regs[17])
#define SC_ARG0(f)  ((f)->regs[10])
#define SC_ARG1(f)  ((f)->regs[11])
#define SC_ARG2(f)  ((f)->regs[12])
#define SC_ARG3(f)  ((f)->regs[13])
#define SC_ARG4(f)  ((f)->regs[14])
#define SC_ARG5(f)  ((f)->regs[15])
#define SC_RET(f)   ((f)->regs[10])
#define SC_SET_RET(f, v) ((f)->regs[10] = (uint64_t)(v))
#define FRAME_PC(f) ((f)->sepc)
#define FRAME_SP(f) ((f)->regs[2])
#define FRAME_IS_SYSCALL(f) ((f)->scause == 8)
#define ARCH_PLATFORM "riscv64"
/* li a7, 139 (rt_sigreturn); ecall */
#define ARCH_SIGTRAMP_CODE 0x08b00893, 0x00000073

static inline void frame_init_user(struct trap_frame *f, uint64_t entry, uint64_t sp) {
    for (uint64_t *p = (uint64_t *)f; p < (uint64_t *)(f + 1); p++) *p = 0;
    f->sepc = entry;
    f->regs[2] = sp;
    f->sstatus = (csr_read(sstatus) & ~(SSTATUS_SPP | SSTATUS_SIE | SSTATUS_FS)) | SSTATUS_SPIE | SSTATUS_FS_INITIAL;
}
/* Re-execute the ecall (for SA_RESTART): a7 still holds the number, a0 was clobbered. */
static inline void frame_restart_syscall(struct trap_frame *f, uint64_t nr) {
    f->regs[17] = nr; f->regs[10] = f->orig_a0; f->sepc -= 4;
}
