#pragma once
#include <arch/trapframe.h>
#define SC_NR(f)    ((f)->regs[8])
#define SC_ARG0(f)  ((f)->regs[0])
#define SC_ARG1(f)  ((f)->regs[1])
#define SC_ARG2(f)  ((f)->regs[2])
#define SC_ARG3(f)  ((f)->regs[3])
#define SC_ARG4(f)  ((f)->regs[4])
#define SC_ARG5(f)  ((f)->regs[5])
#define SC_RET(f)   ((f)->regs[0])
#define SC_SET_RET(f, v) ((f)->regs[0] = (uint64_t)(v))
#define FRAME_PC(f) ((f)->pc)
#define FRAME_SP(f) ((f)->sp)
#define FRAME_IS_SYSCALL(f) (((f)->esr >> 26) == 0x15)
#define ARCH_PLATFORM "aarch64"

static inline void frame_init_user(struct trap_frame *f, uint64_t entry, uint64_t sp) {
    for (uint64_t *p = (uint64_t *)f; p < (uint64_t *)(f + 1); p++) *p = 0;
    f->pc = entry;
    f->sp = sp;
    f->pstate = 0;          /* EL0t, interrupts unmasked */
}
/* Re-execute the svc (for SA_RESTART). */
static inline void frame_restart_syscall(struct trap_frame *f, uint64_t nr) {
    f->regs[8] = nr; f->regs[0] = f->orig_x0; f->pc -= 4;
}
