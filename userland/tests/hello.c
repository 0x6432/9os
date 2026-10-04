/* Freestanding Linux-ABI smoke test: write, fork, wait4, exit. */
typedef long i64;
static i64 sc(i64 n, i64 a, i64 b, i64 c) {
    i64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static i64 slen(const char *s) { i64 n = 0; while (s[n]) n++; return n; }
static void puts_(const char *s) { sc(1, 1, (i64)s, slen(s)); }

void _start(void) {
    puts_("hello from user mode!\n");
    i64 pid = sc(57, 0, 0, 0);
    if (pid == 0) { puts_("  child: running\n"); sc(60, 42, 0, 0); }
    int status = 0;
    i64 w = sc(61, pid, (i64)&status, 0);
    puts_(w == pid && ((status >> 8) & 0xff) == 42 ? "  parent: child exited with 42 - OK\n" : "  parent: wait FAILED\n");
    for (;;) sc(34, 0, 0, 0);   /* pause */
}
