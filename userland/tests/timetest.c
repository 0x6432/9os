/* CPU time accounting: times(), getrusage(SELF/CHILDREN), wait4 rusage, CPU-time clocks,
 * context-switch and page-fault counters, /proc/self/stat. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (c) printf("  [ok] %s\n", #c); else { printf("  [FAIL] %s (line %d)\n", #c, __LINE__); fails++; } } while (0)

static double ns(clockid_t c) { struct timespec t; clock_gettime(c, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static long ms(struct timeval tv) { return tv.tv_sec * 1000 + tv.tv_usec / 1000; }
static volatile unsigned long sink;
static void burn_user(int msec) {
    double end = ns(CLOCK_MONOTONIC) + msec * 1e6;
    while (ns(CLOCK_MONOTONIC) < end) for (int i = 0; i < 20000; i++) sink += i * i;
}

int main(void) {
    double c0 = ns(CLOCK_PROCESS_CPUTIME_ID), t0 = ns(CLOCK_THREAD_CPUTIME_ID);
    burn_user(300);
    double c1 = ns(CLOCK_PROCESS_CPUTIME_ID), t1 = ns(CLOCK_THREAD_CPUTIME_ID);
    printf("process cputime +%.0f ms, thread +%.0f ms\n", (c1 - c0) / 1e6, (t1 - t0) / 1e6);
    CHECK(c1 - c0 > 150e6 && c1 - c0 < 2000e6);
    CHECK(t1 - t0 > 150e6);

    int fd = open("/dev/null", O_WRONLY);
    char b[4096] = {0};
    double e = ns(CLOCK_MONOTONIC) + 300e6;
    while (ns(CLOCK_MONOTONIC) < e) for (int i = 0; i < 50; i++) { write(fd, b, sizeof b); getppid(); }
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    printf("self: utime %ld ms stime %ld ms minflt %ld nvcsw %ld nivcsw %ld\n", ms(ru.ru_utime), ms(ru.ru_stime),
           ru.ru_minflt, ru.ru_nvcsw, ru.ru_nivcsw);
    CHECK(ms(ru.ru_utime) >= 150);
    CHECK(ms(ru.ru_stime) > 0);
    CHECK(ru.ru_minflt > 0);
    long v0 = ru.ru_nvcsw;
    for (int i = 0; i < 5; i++) usleep(1000);
    getrusage(RUSAGE_SELF, &ru);
    CHECK(ru.ru_nvcsw >= v0 + 5);

    struct tms tm;
    clock_t r0 = times(&tm);
    CHECK(r0 != (clock_t)-1);
    CHECK(tm.tms_utime >= 15);

    pid_t c = fork();
    if (c == 0) { burn_user(250); char *m = mmap(0, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); memset(m, 1, 1 << 20); _exit(0); }
    int st;
    struct rusage cr;
    CHECK(wait4(c, &st, 0, &cr) == c);
    printf("child: utime %ld ms stime %ld ms minflt %ld\n", ms(cr.ru_utime), ms(cr.ru_stime), cr.ru_minflt);
    CHECK(ms(cr.ru_utime) >= 120);
    CHECK(cr.ru_minflt >= 256);
    getrusage(RUSAGE_CHILDREN, &ru);
    CHECK(ms(ru.ru_utime) >= 120);
    times(&tm);
    CHECK(tm.tms_cutime >= 12);

    char sbuf[512] = {0};
    int sf = open("/proc/self/stat", O_RDONLY);
    read(sf, sbuf, sizeof sbuf - 1);
    char *p = strrchr(sbuf, ')');
    long f[20] = {0};
    int n = 0;
    for (char *tok = strtok(p + 2, " "); tok && n < 20; tok = strtok(0, " ")) f[n++] = atol(tok);
    /* after "pid (comm)": state=0 ... utime is field 14 → index 11, cutime index 13 */
    printf("/proc/self/stat utime %ld stime %ld cutime %ld\n", f[11], f[12], f[13]);
    CHECK(f[11] >= 15 && f[13] >= 12);

    if (fails) { printf("timetest: %d FAILED\n", fails); return 1; }
    puts("timetest: OK");
    return 0;
}
