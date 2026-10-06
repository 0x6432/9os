/* size_t __copy_user(void *dst, const void *src, size_t n): returns bytes NOT copied; faults
 * are fixed up through __ex_table. Callers bracket it with user_access_begin/end (SSTATUS.SUM). */
.section .text
.balign 4
.global __copy_user
__copy_user:
    or t0, a0, a1
    andi t0, t0, 7
    bnez t0, 3f
    li t1, 8
2:  bltu a2, t1, 3f
10: ld t2, 0(a1)
11: sd t2, 0(a0)
    addi a0, a0, 8
    addi a1, a1, 8
    addi a2, a2, -8
    j 2b
3:  beqz a2, 9f
12: lbu t2, 0(a1)
13: sb t2, 0(a0)
    addi a0, a0, 1
    addi a1, a1, 1
    addi a2, a2, -1
    j 3b
9:  mv a0, a2                      /* normal exit (a2 = 0) and fault fixup */
    ret

.section __ex_table, "a"
.balign 8
    .dword 10b, 9b
    .dword 11b, 9b
    .dword 12b, 9b
    .dword 13b, 9b
