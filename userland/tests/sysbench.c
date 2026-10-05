// sysbench: syscall throughput with 1..N parallel processes (shows BKL contention).
// usage: sysbench [iterations-per-process] [max-procs]
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static double run(int procs, long iters, long nr) {
    double t0 = now();
    for (int i = 0; i < procs; i++)
        if (fork() == 0) {
            for (long k = 0; k < iters; k++) syscall(nr);
            _exit(0);
        }
    for (int i = 0; i < procs; i++) wait(NULL);
    return now() - t0;
}

int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 100000;
    int maxp = argc > 2 ? atoi(argv[2]) : (int)sysconf(_SC_NPROCESSORS_ONLN);
    struct { const char *name; long nr; } calls[] = { { "getpid (lock-free)", SYS_getpid }, { "getppid (BKL)", SYS_getppid } };
    for (unsigned c = 0; c < 2; c++) {
        double base = 0;
        for (int p = 1; p <= maxp; p *= 2) {
            double t = run(p, iters, calls[c].nr);
            double rate = p * iters / t / 1e6;
            if (p == 1) base = rate;
            printf("sysbench: %-20s %2d procs: %6.2f Mcalls/s (x%.2f)\n", calls[c].name, p, rate, rate / base);
        }
    }
    return 0;
}
