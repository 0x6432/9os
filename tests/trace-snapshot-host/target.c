/* Explicit native synthetic tracee, not Konsole or GUI evidence. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0)) {
        /* Some native kernels have no Yama/PR_SET_PTRACER. Still require the
         * real ptrace operation to succeed: never skip its permission gate. */
        if (errno != EINVAL) return 3;
        fprintf(stderr, "target: PR_SET_PTRACER unsupported; ptrace still required\n");
    }
    volatile unsigned long buffer[256];
    for (unsigned i = 0; i < 256; i++) buffer[i] = ~0UL;
    if (!strcmp(argv[1], "many-maps") || !strcmp(argv[1], "too-many-maps")) {
        long page = sysconf(_SC_PAGESIZE);
        if (page <= 0) return 4;
        unsigned count = !strcmp(argv[1], "many-maps") ? 160 : 2200;
        for (unsigned i = 0; i < count; i++) {
            char *p = mmap(NULL, (size_t)page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED || mprotect(p + page, (size_t)page, PROT_READ)) return 4;
        }
    }
    printf("READY %d\n", getpid()); fflush(stdout);
    if (!strcmp(argv[1], "stopped")) raise(SIGSTOP);
    if (!strcmp(argv[1], "busy")) for (;;) buffer[0]++;
    for (;;) pause();
}
