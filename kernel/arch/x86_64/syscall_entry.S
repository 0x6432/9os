.section .text
/* percpu layout: 0 self, 8 kernel_rsp, 16 user_rsp scratch */
.global syscall_entry
syscall_entry:
    swapgs
    movq %rsp, %gs:16
    movq %gs:8, %rsp
    pushq $0x1b            /* ss */
    pushq %gs:16           /* rsp */
    pushq %r11             /* rflags */
    pushq $0x23            /* cs */
    pushq %rcx             /* rip */
    pushq $0               /* error */
    pushq $0x100           /* vector marker */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    xorl %ebp, %ebp
    sti
    call syscall_dispatch
    cli
    jmp trap_return

/* new user threads (fork/clone/exec from kthread) start here with rsp = trap frame */
.global x86_user_return
x86_user_return:
    cli
    movq %rsp, %rdi
    call thread_first_return
    jmp trap_return
