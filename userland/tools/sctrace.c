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
#define NR(r) (r).orig_rax
#define RET(r) (long)(r).rax
#define A0(r) (r).rdi
#define A1(r) (r).rsi
#define A2(r) (r).rdx
#elif defined(__aarch64__)
#define NR(r) (r).regs[8]
#define RET(r) (long)(r).regs[0]
#define A0(r) (r).regs[0]
#define A1(r) (r).regs[1]
#define A2(r) (r).regs[2]
#else
#define NR(r) (r).a7
#define RET(r) (long)(r).a0
#define A0(r) (r).a0
#define A1(r) (r).a1
#define A2(r) (r).a2
#endif

#ifndef __aarch64__
static int getregs(pid_t t, struct user_regs_struct *r) { return ptrace(PTRACE_GETREGS, t, 0, r); }
#else
#include <elf.h>
#include <sys/uio.h>
static int getregs(pid_t t, struct user_regs_struct *r) {
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
            struct user_regs_struct r;
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
