#pragma once
#include <arch/trapframe.h>
#include <arch/gdt.h>

#define SC_NR(f)    ((f)->rax)
#define SC_ARG0(f)  ((f)->rdi)
#define SC_ARG1(f)  ((f)->rsi)
#define SC_ARG2(f)  ((f)->rdx)
#define SC_ARG3(f)  ((f)->r10)
#define SC_ARG4(f)  ((f)->r8)
#define SC_ARG5(f)  ((f)->r9)
#define SC_SET_RET(f, v) ((f)->rax = (uint64_t)(v))
#define FRAME_PC(f) ((f)->rip)
#define FRAME_SP(f) ((f)->rsp)
#define SYSCALL_VECTOR_MARK 0x100

static inline void frame_init_user(struct trap_frame *f, uint64_t entry, uint64_t sp) {
    for (uint64_t *p = (uint64_t *)f; p < (uint64_t *)(f + 1); p++) *p = 0;
    f->rip = entry;
    f->rsp = sp;
    f->cs = USER_CS;
    f->ss = USER_DS;
    f->rflags = 0x202;
}
/* Re-execute the syscall instruction (for SA_RESTART). */
static inline void frame_restart_syscall(struct trap_frame *f, uint64_t nr) { f->rax = nr; f->rip -= 2; }
