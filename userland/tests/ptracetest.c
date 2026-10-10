/* ptrace(2) (M33): TRACEME/ATTACH/SEIZE, PEEK/POKE, register sets, syscall stops with
 * TRACESYSGOOD + PTRACE_GET_SYSCALL_INFO, changing the syscall number, signal suppression and
 * injection with GETSIGINFO, fork/exec/exit events, auto-attach of forked children, software
 * breakpoints poked into text, single step (x86_64/aarch64), INTERRUPT, LISTEN, DETACH, KILL,
 * EXITKILL, waitid CLD_TRAPPED and /proc/<pid>/status TracerPid. */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("ptracetest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); putchar('\n'); fails++; } } while (0)

#ifndef PTRACE_GET_SYSCALL_INFO
#define PTRACE_GET_SYSCALL_INFO 0x420e
#endif
#ifndef NT_ARM_SYSTEM_CALL
#define NT_ARM_SYSTEM_CALL 0x404
#endif
struct sci { uint8_t op, pad[3]; uint32_t arch; uint64_t ip, sp; union { struct { uint64_t nr, args[6]; } entry; struct { int64_t rval; uint8_t is_error; } exit; }; };

#if defined(__x86_64__)
#define NREGS 27
#define REG_PC 16
#define REG_ORIG_NR 15
#define REG_RET 10
#elif defined(__aarch64__)
#define NREGS 34
#define REG_PC 32
#define REG_RET 0
#else
#define NREGS 32
#define REG_PC 0
#define REG_NR 17
#define REG_RET 10
#endif

static int getregs(pid_t p, uint64_t *r) {
    struct iovec iov = { r, NREGS * 8 };
    return ptrace(PTRACE_GETREGSET, p, (void *)NT_PRSTATUS, &iov) ? -errno : (int)iov.iov_len;
}
static int setregs(pid_t p, uint64_t *r) {
    struct iovec iov = { r, NREGS * 8 };
    return ptrace(PTRACE_SETREGSET, p, (void *)NT_PRSTATUS, &iov) ? -errno : 0;
}
static int wstop(pid_t p, int *st) {
    pid_t r = waitpid(p, st, __WALL);
    return r == p && WIFSTOPPED(*st) ? WSTOPSIG(*st) : -1;
}

volatile long shared_word = 0x1122334455667788L;
static volatile int usr1, usr2;
static void on_usr1(int s) { (void)s; usr1++; }
static void on_usr2(int s) { (void)s; usr2++; }
__attribute__((noinline)) int target_fn(int x) { __asm__ volatile("" ::: "memory"); return x * 3 + 1; }

/* 1: TRACEME, memory, registers, syscall stops, syscall rewrite, signals */
static void test_basic(void) {
    pid_t c = fork();
    if (!c) {
        signal(SIGUSR1, on_usr1); signal(SIGUSR2, on_usr2);
        if (ptrace(PTRACE_TRACEME, 0, 0, 0)) _exit(90);
        raise(SIGSTOP);
        if (shared_word != 0x5a5a5a5a5a5a5a5aL) _exit(91);    /* poked by the tracer */
        long r = syscall(SYS_getppid);                       /* rewritten to gettid */
        if (r != getpid()) _exit(92);
        syscall(SYS_getpid);                                  /* traced by PTRACE_SYSCALL */
        raise(SIGUSR1);                                       /* suppressed */
        raise(SIGUSR2);                                       /* replaced by SIGUSR1 */
        if (usr1 != 1 || usr2 != 0) _exit(93);
        _exit(42);
    }
    int st, sig = wstop(c, &st);
    CHECK(sig == SIGSTOP, "TRACEME stop sig=%d st=%x", sig, st);
    char path[64], line[128]; int tracer = -1, tstate = 0;
    snprintf(path, sizeof path, "/proc/%d/status", c);
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        sscanf(line, "TracerPid: %d", &tracer);
        if (!strncmp(line, "State:", 6)) tstate = strchr(line, 't') != 0;
    }
    if (f) fclose(f);
    CHECK(tracer == getpid() && tstate, "TracerPid=%d tstate=%d", tracer, tstate);
    errno = 0;
    long w = ptrace(PTRACE_PEEKDATA, c, (void *)&shared_word, 0);
    CHECK(errno == 0 && w == 0x1122334455667788L, "PEEKDATA %lx errno %d", w, errno);
    CHECK(ptrace(PTRACE_POKEDATA, c, (void *)&shared_word, (void *)0x5a5a5a5a5a5a5a5aL) == 0, "POKEDATA");
    CHECK(shared_word == 0x1122334455667788L, "tracer copy changed");
    errno = 0; ptrace(PTRACE_PEEKDATA, c, (void *)8, 0);
    CHECK(errno == EIO, "PEEKDATA bad address errno=%d", errno);
    uint64_t r[NREGS];
    CHECK(getregs(c, r) == NREGS * 8, "GETREGSET size");
    siginfo_t si;
    CHECK(ptrace(PTRACE_GETSIGINFO, c, 0, &si) == 0 && si.si_signo == SIGSTOP && si.si_code == SI_TKILL && si.si_pid == c,
          "GETSIGINFO SIGSTOP signo=%d code=%d pid=%d", si.si_signo, si.si_code, si.si_pid);
    CHECK(ptrace(PTRACE_SETOPTIONS, c, 0, (void *)PTRACE_O_TRACESYSGOOD) == 0, "SETOPTIONS");
    CHECK(ptrace(PTRACE_CONT, 0x7fffffff, 0, 0) == -1 && errno == ESRCH, "CONT on a non-tracee");
    /* syscall stops until getppid entry: rewrite to gettid */
    int seen_getpid = 0, rewritten = 0, entry = 1;
    for (int i = 0; i < 200; i++) {
        CHECK(ptrace(PTRACE_SYSCALL, c, 0, 0) == 0, "PTRACE_SYSCALL");
        sig = wstop(c, &st);
        if (sig != (SIGTRAP | 0x80)) break;
        struct sci s;
        long n = ptrace(PTRACE_GET_SYSCALL_INFO, c, (void *)sizeof s, &s);
        CHECK(n > 0 && s.op == (entry ? 1 : 2), "GET_SYSCALL_INFO n=%ld op=%d entry=%d", n, s.op, entry);
        if (entry && s.entry.nr == SYS_getppid && !rewritten) {
            getregs(c, r);
#if defined(__x86_64__)
            CHECK((int64_t)r[REG_RET] == -ENOSYS, "rax at entry = %ld", (long)r[REG_RET]);
            r[REG_ORIG_NR] = SYS_gettid; setregs(c, r);
#elif defined(__aarch64__)
            int nr = SYS_gettid; struct iovec iov = { &nr, 4 };
            CHECK(ptrace(PTRACE_SETREGSET, c, (void *)NT_ARM_SYSTEM_CALL, &iov) == 0, "NT_ARM_SYSTEM_CALL");
#else
            r[REG_NR] = SYS_gettid; setregs(c, r);
#endif
            rewritten = 1;
        }
        if (entry && s.entry.nr == SYS_getpid) seen_getpid = 1;
        if (!entry && seen_getpid == 1) { CHECK(s.exit.rval == c && !s.exit.is_error, "getpid exit rval=%ld", (long)s.exit.rval); seen_getpid = 2; }
        if (!entry && seen_getpid == 2) {
            ptrace(PTRACE_CONT, c, 0, 0);       /* run to the next signal */
            sig = wstop(c, &st);
            break;
        }
        entry = !entry;
    }
    CHECK(rewritten && seen_getpid == 2, "syscall stops rewritten=%d getpid=%d", rewritten, seen_getpid);
    CHECK(sig == SIGUSR1, "expected SIGUSR1 stop, got %d", sig);
    CHECK(ptrace(PTRACE_GETSIGINFO, c, 0, &si) == 0 && si.si_signo == SIGUSR1 && si.si_code == SI_TKILL, "GETSIGINFO USR1");
    ptrace(PTRACE_CONT, c, 0, 0);               /* suppress */
    sig = wstop(c, &st);
    CHECK(sig == SIGUSR2, "expected SIGUSR2 stop, got %d", sig);
    ptrace(PTRACE_CONT, c, 0, (void *)SIGUSR1); /* inject a different signal */
    CHECK(waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 42, "basic child exit st=%x", st);
}

/* 2: software breakpoint + single step */
static void test_breakpoint(void) {
    pid_t c = fork();
    if (!c) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        int v = target_fn(4);
        _exit(v == 13 ? 0 : 1);
    }
    int st;
    CHECK(wstop(c, &st) == SIGSTOP, "bp: first stop");
    uintptr_t a = (uintptr_t)target_fn;
    errno = 0;
    long orig = ptrace(PTRACE_PEEKTEXT, c, (void *)a, 0);
    CHECK(errno == 0, "PEEKTEXT");
#if defined(__x86_64__)
    long bp = (orig & ~0xffL) | 0xcc;
#elif defined(__aarch64__)
    long bp = (orig & ~0xffffffffL) | 0xd4200000L;   /* brk #0 */
#else
    long bp = (orig & ~0xffffffffL) | 0x00100073L;   /* ebreak */
#endif
    CHECK(ptrace(PTRACE_POKETEXT, c, (void *)a, (void *)bp) == 0, "POKETEXT into read-only text");
    CHECK(*(volatile long *)a == orig, "tracer text changed (COW broken)");
    ptrace(PTRACE_CONT, c, 0, 0);
    int sig = wstop(c, &st);
    CHECK(sig == SIGTRAP, "breakpoint SIGTRAP got %d", sig);
    uint64_t r[NREGS];
    getregs(c, r);
#if defined(__x86_64__)
    CHECK(r[REG_PC] == a + 1, "pc after int3 %lx want %lx", (long)r[REG_PC], (long)a + 1);
    r[REG_PC] = a; setregs(c, r);
#else
    CHECK(r[REG_PC] == a, "pc at brk %lx want %lx", (long)r[REG_PC], (long)a);
#endif
    CHECK(ptrace(PTRACE_POKETEXT, c, (void *)a, (void *)orig) == 0, "restore text");
#if defined(__x86_64__) || defined(__aarch64__)
    uint64_t pc0 = r[REG_PC];
    int steps = 0;
    for (; steps < 3; steps++) {
        CHECK(ptrace(PTRACE_SINGLESTEP, c, 0, 0) == 0, "SINGLESTEP");
        sig = wstop(c, &st);
        if (sig != SIGTRAP) break;
        siginfo_t si;
        ptrace(PTRACE_GETSIGINFO, c, 0, &si);
        CHECK(si.si_code == TRAP_TRACE, "step si_code %d", si.si_code);
        getregs(c, r);
        CHECK(r[REG_PC] != pc0, "step pc %lx", (long)r[REG_PC]);
        pc0 = r[REG_PC];
    }
    CHECK(steps == 3, "single steps done=%d sig=%d", steps, sig);
#else
    CHECK(ptrace(PTRACE_SINGLESTEP, c, 0, 0) == -1 && errno == EIO, "riscv64 SINGLESTEP -> EIO");
#endif
    ptrace(PTRACE_CONT, c, 0, 0);
    CHECK(waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0, "bp child exit st=%x", st);
}

/* 3: fork/exec/exit events and auto-attached children */
static void test_events(void) {
    pid_t c = fork();
    if (!c) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        pid_t g = fork();
        if (!g) _exit(7);
        int st;
        if (waitpid(g, &st, 0) != g || !WIFEXITED(st) || WEXITSTATUS(st) != 7) _exit(80);
        execl("/bin/sh", "sh", "-c", "exit 5", (char *)0);
        _exit(81);
    }
    int st;
    CHECK(wstop(c, &st) == SIGSTOP, "events: first stop");
    CHECK(ptrace(PTRACE_SETOPTIONS, c, 0, (void *)(PTRACE_O_TRACEFORK | PTRACE_O_TRACEEXEC | PTRACE_O_TRACEEXIT)) == 0, "SETOPTIONS");
    ptrace(PTRACE_CONT, c, 0, 0);
    wstop(c, &st);
    CHECK(st >> 8 == (SIGTRAP | (PTRACE_EVENT_FORK << 8)), "fork event st=%x", st);
    unsigned long g = 0;
    ptrace(PTRACE_GETEVENTMSG, c, 0, &g);
    CHECK(g > 0, "GETEVENTMSG fork pid");
    int gs = wstop((pid_t)g, &st);
    CHECK(gs == SIGSTOP, "auto-attached grandchild stop %d st=%x", gs, st);
    ptrace(PTRACE_CONT, (pid_t)g, 0, 0);
    /* the grandchild's exit event, then its exit (not our child: reported via __WALL) */
    wstop((pid_t)g, &st);
    CHECK(st >> 8 == (SIGTRAP | (PTRACE_EVENT_EXIT << 8)), "grandchild exit event st=%x", st);
    ptrace(PTRACE_CONT, (pid_t)g, 0, 0);
    CHECK(waitpid((pid_t)g, &st, __WALL) == (pid_t)g && WIFEXITED(st) && WEXITSTATUS(st) == 7, "grandchild exit via ptrace st=%x", st);
    ptrace(PTRACE_CONT, c, 0, 0);
    int sig = wstop(c, &st);
    if (sig == SIGCHLD) { ptrace(PTRACE_CONT, c, 0, (void *)SIGCHLD); sig = wstop(c, &st); }
    CHECK(st >> 8 == (SIGTRAP | (PTRACE_EVENT_EXEC << 8)), "exec event st=%x", st);
    ptrace(PTRACE_CONT, c, 0, 0);
    wstop(c, &st);
    CHECK(st >> 8 == (SIGTRAP | (PTRACE_EVENT_EXIT << 8)), "exit event st=%x", st);
    unsigned long ec = 0;
    ptrace(PTRACE_GETEVENTMSG, c, 0, &ec);
    CHECK(WIFEXITED(ec) && WEXITSTATUS(ec) == 5, "exit event msg %lx", ec);
    ptrace(PTRACE_CONT, c, 0, 0);
    CHECK(waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 5, "events child exit st=%x", st);
}

/* 4: SEIZE / INTERRUPT / LISTEN / waitid / KILL; ATTACH / DETACH */
static void test_seize(void) {
    pid_t c = fork();
    if (!c) { for (;;) __asm__ volatile("" ::: "memory"); }
    CHECK(ptrace(PTRACE_SEIZE, c, 0, (void *)PTRACE_O_TRACESYSGOOD) == 0, "SEIZE");
    CHECK(ptrace(PTRACE_SEIZE, c, 0, 0) == -1 && errno == EPERM, "double SEIZE");
    CHECK(ptrace(PTRACE_GETREGSET, c, 0, 0) == -1 && errno == ESRCH, "request on running tracee");
    CHECK(ptrace(PTRACE_INTERRUPT, c, 0, 0) == 0, "INTERRUPT");
    siginfo_t wi; memset(&wi, 0, sizeof wi);
    CHECK(waitid(P_PID, c, &wi, WSTOPPED | WEXITED | WNOWAIT) == 0 && wi.si_code == CLD_TRAPPED && wi.si_pid == c,
          "waitid CLD_TRAPPED code=%d", wi.si_code);
    int st;
    wstop(c, &st);
    CHECK(st >> 16 == PTRACE_EVENT_STOP && WSTOPSIG(st) == SIGTRAP, "interrupt stop st=%x", st);
    /* group stop of a seized tracee: SIGSTOP signal stop, then PTRACE_EVENT_STOP group stop */
    kill(c, SIGSTOP);
    ptrace(PTRACE_CONT, c, 0, 0);
    int sig = wstop(c, &st);
    CHECK(sig == SIGSTOP && st >> 16 == 0, "SIGSTOP delivery stop st=%x", st);
    ptrace(PTRACE_CONT, c, 0, (void *)SIGSTOP);
    wstop(c, &st);
    CHECK(st >> 16 == PTRACE_EVENT_STOP && WSTOPSIG(st) == SIGSTOP, "group stop st=%x", st);
    CHECK(ptrace(PTRACE_LISTEN, c, 0, 0) == 0, "LISTEN");
    CHECK(waitpid(c, &st, WNOHANG | __WALL) == 0, "listening tracee reports nothing");
    kill(c, SIGCONT);
    wstop(c, &st);
    CHECK(st >> 16 == PTRACE_EVENT_STOP, "re-trap after SIGCONT st=%x", st);
    CHECK(ptrace(PTRACE_KILL, c, 0, 0) == 0, "KILL");
    CHECK(waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "killed st=%x", st);

    int pfd[2]; pipe(pfd);
    c = fork();
    if (!c) { char b; close(pfd[1]); while (read(pfd[0], &b, 1) != 1) {} _exit(3); }
    close(pfd[0]);
    CHECK(ptrace(PTRACE_ATTACH, c, 0, 0) == 0, "ATTACH");
    sig = wstop(c, &st);
    CHECK(sig == SIGSTOP, "attach stop %d", sig);
    CHECK(ptrace(PTRACE_DETACH, c, 0, 0) == 0, "DETACH");
    CHECK(ptrace(PTRACE_CONT, c, 0, 0) == -1 && errno == ESRCH, "CONT after DETACH");
    write(pfd[1], "x", 1);
    CHECK(waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 3, "detached child exit st=%x", st);
    close(pfd[1]);
    CHECK(ptrace(PTRACE_ATTACH, getpid(), 0, 0) == -1 && errno == EPERM, "attach to self");
    CHECK(ptrace(PTRACE_ATTACH, 1, 0, 0) == -1 && errno == EPERM, "attach to init");
}

/* 5: PTRACE_O_EXITKILL: the tracee dies with its tracer */
static void test_exitkill(void) {
    pid_t victim = fork();
    if (!victim) { for (;;) pause(); }
    pid_t tracer = fork();
    if (!tracer) _exit(ptrace(PTRACE_SEIZE, victim, 0, (void *)PTRACE_O_EXITKILL) ? 1 : 0);
    int st = 0;
    pid_t w = 0;
    for (int i = 0; i < 300 && !(w = waitpid(tracer, &st, WNOHANG)); i++) usleep(10000);
    CHECK(w == tracer && WIFEXITED(st) && WEXITSTATUS(st) == 0, "tracer w=%d st=%x", w, st);
    for (int i = 0; i < 300 && !(w = waitpid(victim, &st, WNOHANG)); i++) usleep(10000);
    if (!w) {
        kill(victim, SIGKILL); waitpid(victim, &st, 0);
        CHECK(0, "EXITKILL: victim survived its tracer");
    } else CHECK(w == victim && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "EXITKILL victim st=%x", st);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "-x")) { test_exitkill(); return fails != 0; }
    test_basic();      fprintf(stderr, "ptracetest: basic done\n");
    test_breakpoint(); fprintf(stderr, "ptracetest: breakpoint done\n");
    test_events();     fprintf(stderr, "ptracetest: events done\n");
    test_seize();      fprintf(stderr, "ptracetest: seize done\n");
    test_exitkill();   fprintf(stderr, "ptracetest: exitkill done\n");
    if (fails) { printf("ptracetest: %d failures\n", fails); return 1; }
    printf("ptracetest: all passed\n");
    return 0;
}
