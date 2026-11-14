/* Explicit non-setuid synthetic tracee. BusyBox applets can remain
 * non-dumpable after dropping setuid privilege; do not use them here. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "busy") && strcmp(argv[1], "blocked") && strcmp(argv[1], "stopped"))) return 2;
    volatile unsigned long buffer[256];
    for (unsigned i = 0; i < 256; i++) buffer[i] = ~0UL;
    printf("SNAPSHOT_TARGET_READY pid=%d mode=%s\n", getpid(), argv[1]);fflush(stdout);
    if (!strcmp(argv[1], "stopped")) raise(SIGSTOP);
    if (!strcmp(argv[1], "busy")) for (;;) buffer[0]++;
    for (;;) pause();
}
