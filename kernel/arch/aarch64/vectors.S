/* AArch64 exception vectors, trap entry/exit, context switch, FP/SIMD save/restore. */
.macro SAVE_REGS
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mrs x9, sp_el0
    mrs x10, elr_el1
    stp x9, x10, [sp, #248]
    mrs x11, spsr_el1
    mrs x12, esr_el1
    stp x11, x12, [sp, #264]
    mrs x13, far_el1
    str x13, [sp, #280]
    mov x29, xzr
.endm

.section .text
.balign 0x800
.global exception_vectors
exception_vectors:
    .balign 0x80
    b bad_vector
    .balign 0x80
    b bad_vector
    .balign 0x80
    b bad_vector
    .balign 0x80
    b bad_vector
    .balign 0x80
    b el1_sync
    .balign 0x80
    b el1_irq
    .balign 0x80
    b bad_vector
    .balign 0x80
    b bad_vector
    .balign 0x80
    b el0_sync
    .balign 0x80
    b el0_irq
    .balign 0x80
    b bad_vector
    .balign 0x80
    b bad_vector
    .balign 0x80
    b bad_vector
    .balign 0x80
    b bad_vector
    .balign 0x80
    b bad_vector
    .balign 0x80
    b bad_vector

el1_sync:
    sub sp, sp, #304
    SAVE_REGS
    mov x0, sp
    mov x1, #0
    bl trap_dispatch
    b trap_return

el1_irq:
    sub sp, sp, #304
    SAVE_REGS
    mov x0, sp
    mov x1, #1
    bl trap_dispatch
    b trap_return

el0_sync:
    sub sp, sp, #304
    SAVE_REGS
    mov x0, sp
    mov x1, #2
    bl trap_dispatch
    b trap_return

el0_irq:
    sub sp, sp, #304
    SAVE_REGS
    mov x0, sp
    mov x1, #3
    bl trap_dispatch
    b trap_return

bad_vector:
    sub sp, sp, #304
    SAVE_REGS
    mov x0, sp
    mov x1, #9
    bl trap_dispatch
    b .

/* sp = trap frame */
.global trap_return
trap_return:
    msr daifset, #2
    ldp x9, x10, [sp, #248]
    msr sp_el0, x9
    msr elr_el1, x10
    ldr x11, [sp, #264]
    msr spsr_el1, x11
    tst x11, #0xf
    b.ne 1f
    ic iallu                        /* user code may have been written through the HHDM */
    dsb ish
    isb
1:
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #304
    eret

/* void a64_switch_stack(uint64_t *old_sp, uint64_t new_sp) */
.global a64_switch_stack
a64_switch_stack:
    sub sp, sp, #96
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    mov x9, sp
    str x9, [x0]
    mov sp, x1
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #96
    ret

/* first return target of a new kernel thread: x19 = fn, x20 = arg */
.global a64_thread_trampoline
a64_thread_trampoline:
    msr daifclr, #2
    mov x0, x20
    blr x19
    bl thread_exit

/* new user threads start here with sp = their user trap frame */
.global a64_user_return
a64_user_return:
    msr daifset, #2
    mov x0, sp
    bl user_return_work
    b trap_return

.global arch_enter_user
arch_enter_user:
    msr daifset, #2
    mov sp, x0
    b trap_return

/* fp_save(uint64_t buf[66]) / fp_restore: q0-q31, fpsr, fpcr */
.global fp_save
fp_save:
    stp q0, q1, [x0, #0]
    stp q2, q3, [x0, #32]
    stp q4, q5, [x0, #64]
    stp q6, q7, [x0, #96]
    stp q8, q9, [x0, #128]
    stp q10, q11, [x0, #160]
    stp q12, q13, [x0, #192]
    stp q14, q15, [x0, #224]
    stp q16, q17, [x0, #256]
    stp q18, q19, [x0, #288]
    stp q20, q21, [x0, #320]
    stp q22, q23, [x0, #352]
    stp q24, q25, [x0, #384]
    stp q26, q27, [x0, #416]
    stp q28, q29, [x0, #448]
    stp q30, q31, [x0, #480]
    mrs x9, fpsr
    mrs x10, fpcr
    stp x9, x10, [x0, #512]
    ret

.global fp_restore
fp_restore:
    ldp q0, q1, [x0, #0]
    ldp q2, q3, [x0, #32]
    ldp q4, q5, [x0, #64]
    ldp q6, q7, [x0, #96]
    ldp q8, q9, [x0, #128]
    ldp q10, q11, [x0, #160]
    ldp q12, q13, [x0, #192]
    ldp q14, q15, [x0, #224]
    ldp q16, q17, [x0, #256]
    ldp q18, q19, [x0, #288]
    ldp q20, q21, [x0, #320]
    ldp q22, q23, [x0, #352]
    ldp q24, q25, [x0, #384]
    ldp q26, q27, [x0, #416]
    ldp q28, q29, [x0, #448]
    ldp q30, q31, [x0, #480]
    ldp x9, x10, [x0, #512]
    msr fpsr, x9
    msr fpcr, x10
    ret

/* Limine entry: make sure we run on SP_EL1 (EL1h), then enter C. */
.global kmain_entry
kmain_entry:
    mov x9, sp
    msr spsel, #1
    mov sp, x9
    mov x29, xzr
    mov x30, xzr
    bl kmain
2:  wfi
    b 2b
