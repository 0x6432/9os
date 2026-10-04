.section .text
/* void x86_switch_stack(uint64_t *old_rsp, uint64_t new_rsp) */
.global x86_switch_stack
x86_switch_stack:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

/* First return target of a new thread: r12 = fn, r13 = arg */
.global x86_thread_trampoline
x86_thread_trampoline:
    call sched_finish_switch
    sti
    movq %r13, %rdi
    callq *%r12
    call thread_exit

/* void arch_enter_user(struct trap_frame *f) */
.global arch_enter_user
arch_enter_user:
    cli
    movq %rdi, %rsp
    jmp trap_return

/* Limine MP entry for application processors: rdi = struct limine_mp_info * */
.global x86_ap_trampoline
x86_ap_trampoline:
    movq 24(%rdi), %rax        /* extra_argument = struct cpu * */
    movq 8(%rax), %rsp         /* cpu->kernel_sp */
    xorl %ebp, %ebp
    call x86_ap_entry
1:  cli
    hlt
    jmp 1b
