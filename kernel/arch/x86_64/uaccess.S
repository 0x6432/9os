/*
 * size_t __copy_user(void *dst, const void *src, size_t n): the only code that touches user
 * memory for copy_{to,from}_user/strncpy_from_user. Returns the number of bytes NOT copied.
 * A fault inside it is redirected through __ex_table to the fixup, never a kernel panic.
 * Callers bracket it with user_access_begin/end (STAC/CLAC under SMAP).
 */
.section .text
.global __copy_user
__copy_user:
    movq %rdx, %rcx
    shrq $3, %rcx
    andl $7, %edx
1:  rep movsq
    movq %rdx, %rcx
2:  rep movsb
    xorl %eax, %eax
    ret
3:  leaq (%rdx,%rcx,8), %rax       /* faulted in the qword loop */
    ret
4:  movq %rcx, %rax                /* faulted in the byte tail */
    ret

.section __ex_table, "a"
.balign 8
    .quad 1b, 3b
    .quad 2b, 4b
