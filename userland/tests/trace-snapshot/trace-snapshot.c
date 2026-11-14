/* Bounded, explicitly timing-perturbing single-thread failure diagnostic.
 * No unwinder or application-consumption proof. Never use to rescue a gate.
 * Public Linux ptrace ABI only; no writes, stepping or EXITKILL. */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__x86_64__)
#define NREGS 27
#define PC 16
#define SP 19
#define ARCH "x86_64"
#elif defined(__aarch64__)
#define NREGS 34
#define PC 32
#define SP 31
#define ARCH "aarch64"
#elif defined(__riscv) && __riscv_xlen == 64
#define NREGS 32
#define PC 0
#define SP 2
#define ARCH "riscv64"
#else
#error unsupported register ABI
#endif

static int64_t milliseconds(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int stop_wait(pid_t tid, int *status) {
    int64_t start = milliseconds();
    if (start < 0) return -1;
    for (unsigned i = 0; i < 400; i++) {
        pid_t p = waitpid(tid, status, __WALL | WNOHANG);
        if (p == tid) return WIFSTOPPED(*status) ? 0 : -1;
        if (p < 0 && errno != EINTR) return -1;
        int64_t now = milliseconds();
        if (now < 0 || now - start >= 2000) { errno = ETIMEDOUT; return -1; }
        struct timespec pause = {0, 5000000};
        nanosleep(&pause, NULL);
    }
    errno = ETIMEDOUT;
    return -1;
}
static int maps(pid_t tid, uint64_t pc, uint64_t sp) {
    char path[64], line[1024];
    /* Buffered scanning, not thousands of flushed MAP writes while stopped.
     * Only print the real mappings containing PC/SP; never infer a module
     * from a nearby address or treat raw stack words as unwound frames. */
    static char read_buffer[65536];
    snprintf(path, sizeof path, "/proc/%d/maps", tid);
    FILE *f = fopen(path, "r");
    if (!f) { printf("TRACE_SNAPSHOT_MAPS_ERROR errno=%d\n", errno); return -1; }
    if (setvbuf(f, read_buffer, _IOFBF, sizeof read_buffer)) {
        fclose(f);
        puts("TRACE_SNAPSHOT_MAPS_ERROR reason=buffer");
        return -1;
    }
    puts("TRACE_SNAPSHOT_MAPS_BEGIN");
    unsigned lines = 0, selected = 0;
    int pc_found = 0, sp_found = 0, malformed = 0;
    while (lines < 4096 && fgets(line, sizeof line, f)) {
        unsigned long long start, end;
        char perm[5];
        lines++;
        if (!strchr(line, '\n') || sscanf(line, "%llx-%llx %4s", &start, &end, perm) != 3 ||
            start >= end || strlen(perm) != 4 ||
            !strchr("r-", perm[0]) || !strchr("w-", perm[1]) ||
            !strchr("x-", perm[2]) || !strchr("ps", perm[3])) {
            malformed = 1;
            break;
        }
        int has_pc = pc >= start && pc < end, has_sp = sp >= start && sp < end;
        if (has_pc || has_sp) {
            if (++selected > 2 || (has_pc && pc_found) || (has_sp && sp_found) ||
                (has_pc && perm[2] != 'x') || (has_sp && perm[0] != 'r')) {
                malformed = 1; /* overlapping/changed mappings cannot be trusted */
                break;
            }
            printf("MAP %s", line);
            pc_found |= has_pc;
            sp_found |= has_sp;
        }
    }
    int truncated = !feof(f), error = ferror(f);
    fclose(f);
    printf("TRACE_SNAPSHOT_MAPS_END chunks=%u selected=%u pc=%d sp=%d truncated=%d error=%d malformed=%d\n",
           lines, selected, pc_found, sp_found, truncated, error, malformed);
    return error || truncated || malformed || !pc_found || !sp_found ? -1 : 0;
}
int main(int argc, char **argv) {
    char *end;
    if (argc != 2 || !argv[1][0]) return 2;
    for (const char *s = argv[1]; *s; s++) if (*s < '0' || *s > '9') return 2;
    errno = 0;
    long n = strtol(argv[1], &end, 10);
    if (errno || *end || n <= 0 || n > INT_MAX || n == getpid()) return 2;
    pid_t tid = (pid_t)n;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("TRACE_SNAPSHOT_BEGIN tid=%d arch=%s timing_perturbed=1\n", tid, ARCH);
    if (ptrace(PTRACE_SEIZE, tid, 0, 0)) {
        printf("TRACE_SNAPSHOT_ERROR phase=seize errno=%d\n", errno);
        return 1;
    }
    int status = 0, stopped = 0, result = 1;
    if (ptrace(PTRACE_INTERRUPT, tid, 0, 0)) {
        printf("TRACE_SNAPSHOT_ERROR phase=interrupt errno=%d\n", errno);
        goto cleanup;
    }
    if (stop_wait(tid, &status)) {
        printf("TRACE_SNAPSHOT_ERROR phase=wait errno=%d\n", errno);
        goto cleanup;
    }
    stopped = 1;
    /* Only our interrupt stop is usable. Real signal/group stops must not be
     * swallowed; preserve their signal on detach and report no snapshot. */
    if ((unsigned)status >> 16 != 128 || WSTOPSIG(status) != SIGTRAP) {
        printf("TRACE_SNAPSHOT_ERROR phase=unexpected-stop status=%x\n", status);
        goto cleanup;
    }
    uint64_t regs[NREGS] = {0};
    struct iovec io = {regs, sizeof regs};
    if (ptrace(PTRACE_GETREGSET, tid, (void *)(uintptr_t)NT_PRSTATUS, &io) ||
        io.iov_len != sizeof regs) {
        printf("TRACE_SNAPSHOT_ERROR phase=registers errno=%d size=%zu\n", errno, io.iov_len);
        goto cleanup;
    }
    printf("TRACE_SNAPSHOT_REGS pc=%016" PRIx64 " sp=%016" PRIx64 " bytes=%zu\n",
           regs[PC], regs[SP], io.iov_len);
    for (unsigned i = 0; i < NREGS; i++) printf("REG %u %016" PRIx64 "\n", i, regs[i]);
    result = maps(tid, regs[PC], regs[SP]) ? 1 : 0;
    /* Raw words may be non-address data: NOT unwound frames. Never fabricate
     * zeros for unreadable memory. At most 32 words, one stopped thread. */
    for (unsigned i = 0; i < 32; i++) {
        if (regs[SP] > UINT64_MAX - i * sizeof(long)) { result = 1; break; }
        uint64_t address = regs[SP] + i * sizeof(long);
        errno = 0;
        long word = ptrace(PTRACE_PEEKDATA, tid, (void *)(uintptr_t)address, 0);
        if (errno) {
            printf("TRACE_SNAPSHOT_STACK_ERROR index=%u errno=%d\n", i, errno);
            result = 1;
            break;
        }
        printf("STACK %016" PRIx64 " %016" PRIx64 "\n", address, (uint64_t)word);
    }
cleanup:
    if (stopped) {
        int sig = ((unsigned)status >> 16 == 128 && WSTOPSIG(status) == SIGTRAP)
                  ? 0 : WSTOPSIG(status);
        if (ptrace(PTRACE_DETACH, tid, 0, (void *)(intptr_t)sig)) {
            printf("TRACE_SNAPSHOT_ERROR phase=detach errno=%d\n", errno);
            result = 1;
        } else printf("TRACE_SNAPSHOT_DETACHED signal=%d\n", sig);
    } else {
        /* No EXITKILL: tracer exit releases a running/unresponsive tracee.
         * Cannot claim clean explicit detach without an observed stop. */
        puts("TRACE_SNAPSHOT_RELEASE_ON_TRACER_EXIT");
    }
    printf("TRACE_SNAPSHOT_END result=%d\n", result);
    return result;
}
