/* size_t __copy_user(void *dst, const void *src, size_t n): returns bytes NOT copied; faults
 * are fixed up through __ex_table. Callers bracket it with user_access_begin/end (PSTATE.PAN). */
.section .text
.balign 4
.global __copy_user
__copy_user:
    orr x9, x0, x1
    tst x9, #7
    b.ne 3f
2:  cmp x2, #8
    b.lo 3f
10: ldr x10, [x1], #8
11: str x10, [x0], #8
    sub x2, x2, #8
    b 2b
3:  cbz x2, 9f
12: ldrb w10, [x1], #1
13: strb w10, [x0], #1
    sub x2, x2, #1
    b 3b
9:  mov x0, x2                     /* normal exit (x2 = 0) and fault fixup */
    ret

.section __ex_table, "a"
.balign 8
    .quad 10b, 9b
    .quad 11b, 9b
    .quad 12b, 9b
    .quad 13b, 9b
