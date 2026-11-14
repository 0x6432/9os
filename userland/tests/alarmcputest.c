/* Busy user-mode alarm delivery on CPU0 and a secondary CPU. No syscalls in
 * the wait loop: success must come from timer-IRQ user-return signal work. */
#define _GNU_SOURCE
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
static volatile sig_atomic_t hits;
static void handle(int signal) { if (signal == SIGALRM) hits++; }
int main(void) {
    cpu_set_t allowed, one;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof allowed, &allowed)) { perror("getaffinity"); return 1; }
    int first = -1, last = -1;
    for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &allowed)) {
        if (first < 0) first = i;
        last = i;
    }
    if (first < 0) return 1;
    struct sigaction action = {0};
    action.sa_handler = handle;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL)) { perror("sigaction"); return 1; }
    int cpus[] = {first, last}, count = first == last ? 1 : 2;
    for (int i = 0; i < count; i++) {
        CPU_ZERO(&one); CPU_SET(cpus[i], &one);
        if (sched_setaffinity(0, sizeof one, &one)) { perror("setaffinity"); return 1; }
        hits = 0;
        alarm(1);
        while (!hits) { /* Deliberately no clock, yield, sleep or syscall. */ }
        alarm(0);
        if (hits != 1 || sched_getcpu() != cpus[i]) return 1;
        printf("alarmcputest: delivered SIGALRM in busy user loop on cpu%d\n", cpus[i]);
    }
    if (sched_setaffinity(0, sizeof allowed, &allowed)) return 1;
    puts("alarmcputest: PASSED busy user timer delivery on allowed endpoint CPUs");
    return 0;
}
