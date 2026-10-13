/* sctrace PID [COUNT]: attach with ptrace and print the next COUNT system calls of every thread
 * in the process (number, first args, result), then detach. A minimal strace for debugging. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__x86_64__)
typedef struct user_regs_struct regs_t;
#define NR(r) (r).orig_rax
#define RET(r) (long)(r).rax
#define A0(r) (r).rdi
#define A1(r) (r).rsi
#define A2(r) (r).rdx
static int getregs(pid_t t, regs_t *r) { return ptrace(PTRACE_GETREGS, t, 0, r); }
#else
#include <elf.h>
#include <sys/uio.h>
/* NT_PRSTATUS layout: aarch64 x0..x30,sp,pc,pstate; riscv64 pc,ra,sp,gp,tp,t0-t2,s0,s1,a0..a7,... */
typedef struct { unsigned long r[64]; } regs_t;
#if defined(__aarch64__)
#define NR(x) (x).r[8]
#define AR(x, i) (x).r[i]
#else
#define NR(x) (x).r[17]
#define AR(x, i) (x).r[10 + (i)]
#endif
#define RET(x) (long)AR(x, 0)
#define A0(x) AR(x, 0)
#define A1(x) AR(x, 1)
#define A2(x) AR(x, 2)
static int getregs(pid_t t, regs_t *r) {
    struct iovec io = { r, sizeof *r };
    return ptrace(PTRACE_GETREGSET, t, (void *)NT_PRSTATUS, &io);
}
#endif

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: sctrace PID [COUNT]\n"); return 2; }
    int pid = atoi(argv[1]), count = argc > 2 ? atoi(argv[2]) : 200;
    char path[64]; snprintf(path, sizeof path, "/proc/%d/task", pid);
    pid_t tids[64]; int nt = 0;
    DIR *d = opendir(path);
    if (d) { struct dirent *e; while ((e = readdir(d)) && nt < 64) if (e->d_name[0] != '.') tids[nt++] = atoi(e->d_name); closedir(d); }
    if (!nt) tids[nt++] = pid;
    for (int i = 0; i < nt; i++) {
        if (ptrace(PTRACE_SEIZE, tids[i], 0, PTRACE_O_TRACESYSGOOD)) { perror("PTRACE_SEIZE"); continue; }
        ptrace(PTRACE_INTERRUPT, tids[i], 0, 0);
    }
    char insys[65536 / 8] = {0};
    for (int n = 0; n < count * 2;) {
        int st; pid_t t = waitpid(-1, &st, __WALL);
        if (t < 0) { if (errno == EINTR) continue; break; }
        if (WIFEXITED(st) || WIFSIGNALED(st)) continue;
        int sig = 0;
        if (WIFSTOPPED(st) && WSTOPSIG(st) == (SIGTRAP | 0x80)) {
            regs_t r;
            if (!getregs(t, &r)) {
                int in = insys[t / 8] >> (t % 8) & 1;
                if (!in) printf("[%d] %3lu(%lx, %lx, %lx)", t, (unsigned long)NR(r), (unsigned long)A0(r), (unsigned long)A1(r), (unsigned long)A2(r));
                else printf(" = %ld\n", RET(r));
                insys[t / 8] ^= 1 << (t % 8);
            }
            n++;
        } else if (WIFSTOPPED(st) && WSTOPSIG(st) != SIGTRAP && (st >> 16) == 0) {
            sig = WSTOPSIG(st);
        }
        ptrace(PTRACE_SYSCALL, t, 0, sig);
    }
    fflush(stdout);
    for (int i = 0; i < nt; i++) { ptrace(PTRACE_INTERRUPT, tids[i], 0, 0); }
    for (int i = 0; i < nt; i++) { int st; waitpid(tids[i], &st, __WALL); ptrace(PTRACE_DETACH, tids[i], 0, 0); }
    printf("\n");
    return 0;
}
