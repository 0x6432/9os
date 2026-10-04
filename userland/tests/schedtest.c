/* schedtest: wake-up latency of an interactive task while CPU-bound hogs compete.
 * Under MLFQ the hogs sink to low-priority levels, so the sleeper (which never uses its
 * slice) should be scheduled almost immediately; under round robin it waits behind them. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char **argv) {
    int hogs = argc > 1 ? atoi(argv[1]) : 2 * (int)sysconf(_SC_NPROCESSORS_ONLN);
    int iters = argc > 2 ? atoi(argv[2]) : 200;
    FILE *f = fopen("/proc/sched", "r"); char pol[64] = "?";
    if (f) { fscanf(f, "policy: %63s", pol); fclose(f); }
    printf("schedtest: policy %s, %d CPU hogs, %d sleeps of 2 ms\n", pol, hogs, iters);
    pid_t pids[64];
    for (int i = 0; i < hogs && i < 64; i++)
        if ((pids[i] = fork()) == 0) { volatile unsigned long x = 0; for (;;) x++; }
    usleep(300000);                      /* let the hogs use up their allotment */
    double sum = 0, worst = 0;
    for (int i = 0; i < iters; i++) {
        double t0 = now();
        usleep(2000);
        double lat = (now() - t0) * 1e3 - 2.0;
        sum += lat; if (lat > worst) worst = lat;
    }
    for (int i = 0; i < hogs && i < 64; i++) kill(pids[i], SIGKILL);
    while (wait(NULL) > 0) ;
    printf("  wake-up latency: avg %.2f ms, worst %.2f ms\n", sum / iters, worst);
    return 0;
}
