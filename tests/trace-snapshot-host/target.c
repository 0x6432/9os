/* Explicit native synthetic tracee, not Konsole or GUI evidence. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
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
    printf("READY %d\n", getpid()); fflush(stdout);
    if (!strcmp(argv[1], "stopped")) raise(SIGSTOP);
    if (!strcmp(argv[1], "busy")) for (;;) buffer[0]++;
    for (;;) pause();
}
