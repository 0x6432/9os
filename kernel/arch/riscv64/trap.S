/* RISC-V S-mode trap entry/exit, context switch, FPU save/restore. */
#define FRAME 304

.section .text
.balign 4
.global trap_entry
trap_entry:
    /* user mode: sscratch = current thread; kernel mode: sscratch = 0, tp = current thread */
    csrrw tp, sscratch, tp
    bnez tp, 1f
    csrrw tp, sscratch, tp          /* trap from kernel: restore tp, sscratch stays 0 */
    addi sp, sp, -FRAME
    sd t0, 40(sp)
    addi t0, sp, FRAME
    sd t0, 16(sp)
    sd tp, 32(sp)
    j 2f
1:  sd sp, 16(tp)                   /* trap from user: frame sits at the top of the kernel stack */
    ld sp, 8(tp)
    addi sp, sp, -FRAME
    sd t0, 40(sp)
    ld t0, 16(tp)
    sd t0, 16(sp)
    csrr t0, sscratch               /* user tp */
    sd t0, 32(sp)
    csrw sscratch, zero
    li t0, 0x40000                  /* SSTATUS.SUM: the user-access window starts closed */
    csrc sstatus, t0
2:
    sd x1, 8(sp)
    sd x3, 24(sp)
    sd x6, 48(sp)
    sd x7, 56(sp)
    sd x8, 64(sp)
    sd x9, 72(sp)
    sd x10, 80(sp)
    sd x11, 88(sp)
    sd x12, 96(sp)
    sd x13, 104(sp)
    sd x14, 112(sp)
    sd x15, 120(sp)
    sd x16, 128(sp)
    sd x17, 136(sp)
    sd x18, 144(sp)
    sd x19, 152(sp)
    sd x20, 160(sp)
    sd x21, 168(sp)
    sd x22, 176(sp)
    sd x23, 184(sp)
    sd x24, 192(sp)
    sd x25, 200(sp)
    sd x26, 208(sp)
    sd x27, 216(sp)
    sd x28, 224(sp)
    sd x29, 232(sp)
    sd x30, 240(sp)
    sd x31, 248(sp)
    csrr t0, sepc
    sd t0, 256(sp)
    csrr t0, sstatus
    sd t0, 264(sp)
    csrr t0, scause
    sd t0, 272(sp)
    csrr t0, stval
    sd t0, 280(sp)
    mv s0, zero                     /* terminate frame-pointer chains */
    mv a0, sp
    call trap_dispatch
    /* fall through */

/* sp = trap frame */
.global trap_return
trap_return:
    csrci sstatus, 2
    ld t0, 264(sp)
    andi t1, t0, 0x100              /* SPP: returning to kernel? */
    bnez t1, 3f
    csrw sscratch, tp               /* current thread, for the next trap from user mode */
    fence.i
3:  csrw sstatus, t0
    ld t0, 256(sp)
    csrw sepc, t0
    ld x1, 8(sp)
    ld x3, 24(sp)
    ld x4, 32(sp)
    ld x5, 40(sp)
    ld x6, 48(sp)
    ld x7, 56(sp)
    ld x8, 64(sp)
    ld x9, 72(sp)
    ld x10, 80(sp)
    ld x11, 88(sp)
    ld x12, 96(sp)
    ld x13, 104(sp)
    ld x14, 112(sp)
    ld x15, 120(sp)
    ld x16, 128(sp)
    ld x17, 136(sp)
    ld x18, 144(sp)
    ld x19, 152(sp)
    ld x20, 160(sp)
    ld x21, 168(sp)
    ld x22, 176(sp)
    ld x23, 184(sp)
    ld x24, 192(sp)
    ld x25, 200(sp)
    ld x26, 208(sp)
    ld x27, 216(sp)
    ld x28, 224(sp)
    ld x29, 232(sp)
    ld x30, 240(sp)
    ld x31, 248(sp)
    ld x2, 16(sp)
    sret

/* void riscv_switch_stack(uint64_t *old_sp, uint64_t new_sp) */
.global riscv_switch_stack
riscv_switch_stack:
    addi sp, sp, -112
    sd ra, 0(sp)
    sd s0, 8(sp)
    sd s1, 16(sp)
    sd s2, 24(sp)
    sd s3, 32(sp)
    sd s4, 40(sp)
    sd s5, 48(sp)
    sd s6, 56(sp)
    sd s7, 64(sp)
    sd s8, 72(sp)
    sd s9, 80(sp)
    sd s10, 88(sp)
    sd s11, 96(sp)
    sd sp, 0(a0)
    mv sp, a1
    ld ra, 0(sp)
    ld s0, 8(sp)
    ld s1, 16(sp)
    ld s2, 24(sp)
    ld s3, 32(sp)
    ld s4, 40(sp)
    ld s5, 48(sp)
    ld s6, 56(sp)
    ld s7, 64(sp)
    ld s8, 72(sp)
    ld s9, 80(sp)
    ld s10, 88(sp)
    ld s11, 96(sp)
    addi sp, sp, 112
    ret

/* first return target of a new kernel thread: s1 = fn, s2 = arg */
.global riscv_thread_trampoline
riscv_thread_trampoline:
    call sched_finish_switch
    csrsi sstatus, 2
    mv a0, s2
    jalr s1
    call thread_exit

/* new user threads start here with sp = their user trap frame */
.global riscv_user_return
riscv_user_return:
    csrci sstatus, 2
    mv a0, sp
    call thread_first_return
    j trap_return

/* void arch_enter_user(struct trap_frame *f) */
.global arch_enter_user
arch_enter_user:
    csrci sstatus, 2
    mv sp, a0
    j trap_return

/* fp_save(uint64_t buf[33]) / fp_restore: f0-f31 then fcsr (Linux __riscv_d_ext_state layout) */
.global fp_save
fp_save:
    fsd f0, 0(a0)
    fsd f1, 8(a0)
    fsd f2, 16(a0)
    fsd f3, 24(a0)
    fsd f4, 32(a0)
    fsd f5, 40(a0)
    fsd f6, 48(a0)
    fsd f7, 56(a0)
    fsd f8, 64(a0)
    fsd f9, 72(a0)
    fsd f10, 80(a0)
    fsd f11, 88(a0)
    fsd f12, 96(a0)
    fsd f13, 104(a0)
    fsd f14, 112(a0)
    fsd f15, 120(a0)
    fsd f16, 128(a0)
    fsd f17, 136(a0)
    fsd f18, 144(a0)
    fsd f19, 152(a0)
    fsd f20, 160(a0)
    fsd f21, 168(a0)
    fsd f22, 176(a0)
    fsd f23, 184(a0)
    fsd f24, 192(a0)
    fsd f25, 200(a0)
    fsd f26, 208(a0)
    fsd f27, 216(a0)
    fsd f28, 224(a0)
    fsd f29, 232(a0)
    fsd f30, 240(a0)
    fsd f31, 248(a0)
    frcsr t0
    sw t0, 256(a0)
    ret

.global fp_restore
fp_restore:
    fld f0, 0(a0)
    fld f1, 8(a0)
    fld f2, 16(a0)
    fld f3, 24(a0)
    fld f4, 32(a0)
    fld f5, 40(a0)
    fld f6, 48(a0)
    fld f7, 56(a0)
    fld f8, 64(a0)
    fld f9, 72(a0)
    fld f10, 80(a0)
    fld f11, 88(a0)
    fld f12, 96(a0)
    fld f13, 104(a0)
    fld f14, 112(a0)
    fld f15, 120(a0)
    fld f16, 128(a0)
    fld f17, 136(a0)
    fld f18, 144(a0)
    fld f19, 152(a0)
    fld f20, 160(a0)
    fld f21, 168(a0)
    fld f22, 176(a0)
    fld f23, 184(a0)
    fld f24, 192(a0)
    fld f25, 200(a0)
    fld f26, 208(a0)
    fld f27, 216(a0)
    fld f28, 224(a0)
    fld f29, 232(a0)
    fld f30, 240(a0)
    fld f31, 248(a0)
    lw t0, 256(a0)
    fscsr t0
    ret

/* Limine MP entry for application harts: a0 = struct limine_mp_info * */
.global riscv_ap_trampoline
riscv_ap_trampoline:
    ld t0, 32(a0)              /* extra_argument = struct cpu * */
    ld sp, 8(t0)               /* cpu->kernel_sp */
    mv s0, zero
    mv tp, zero
    call riscv_ap_entry
1:  wfi
    j 1b
