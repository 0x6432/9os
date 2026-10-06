/* M27 hardening: ASLR (PIE, stack, mmap, brk, libc), AT_RANDOM, EFAULT instead of kernel
 * faults for bad user pointers (exception-table fixups), user-access gating not breaking
 * syscalls, W^X accounting, kernel W^X audit and stack protector state (/proc/hardening). */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static long hard_val(const char *key) {
    FILE *f = fopen("/proc/hardening", "r");
    if (!f) return -1;
    char k[64], v[128];
    long r = -1;
    while (fscanf(f, "%63s %127[^\n]", k, v) == 2) if (!strcmp(k, key)) { r = strtol(v, 0, 0); break; }
    fclose(f);
    return r;
}

static int child(void) {
    int local;
    void *m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned long *rnd = (unsigned long *)getauxval(AT_RANDOM);
    printf("%lx %lx %lx %lx %lx %lx\n", (unsigned long)&child, (unsigned long)&local, (unsigned long)m,
           (unsigned long)sbrk(0), (unsigned long)&printf, rnd ? rnd[0] : 0);
    return 0;
}

static void aslr(void) {
    unsigned long v[3][6];
    for (int i = 0; i < 3; i++) {
        int p[2];
        pipe(p);
        pid_t pid = fork();
        if (!pid) { dup2(p[1], 1); close(p[0]); execl("/bin/hardentest", "hardentest", "child", (char *)0); _exit(127); }
        close(p[1]);
        char buf[256] = {0};
        int n = 0, r;
        while ((r = read(p[0], buf + n, sizeof buf - 1 - n)) > 0) n += r;
        close(p[0]);
        waitpid(pid, 0, 0);
        CHECK(sscanf(buf, "%lx %lx %lx %lx %lx %lx", &v[i][0], &v[i][1], &v[i][2], &v[i][3], &v[i][4], &v[i][5]) == 6,
              "child output '%s'", buf);
    }
    const char *names[] = { "PIE text", "stack", "mmap", "brk", "libc", "AT_RANDOM" };
    long on = hard_val("randomize_va_space");
    for (int k = 0; k < 6; k++) {
        int same = v[0][k] == v[1][k] && v[1][k] == v[2][k];
        printf("  %-9s %lx %lx %lx\n", names[k], v[0][k], v[1][k], v[2][k]);
        if (k == 5 || on) CHECK(!same, "%s not randomized", names[k]);
    }
    CHECK(v[0][0] >= 0x400000 && v[0][0] < 0x7f0000000000UL, "PIE text outside its window");
}

static void efault(void) {
    long fix0 = hard_val("extable_fixups");
    char *bad = (char *)0x1000, *kern = (char *)0xffff800000001000UL;
    errno = 0; CHECK(write(1, bad, 16) < 0 && errno == EFAULT, "write(bad) errno %d", errno);
    errno = 0; CHECK(write(1, kern, 16) < 0 && errno == EFAULT, "write(kernel addr) errno %d", errno);
    errno = 0; CHECK(open(bad, O_RDONLY) < 0 && errno == EFAULT, "open(bad) errno %d", errno);
    errno = 0; CHECK(open(kern, O_RDONLY) < 0 && errno == EFAULT, "open(kernel addr) errno %d", errno);
    struct stat st;
    /* raw syscalls: libc's stat() may bounce through its own buffer */
    errno = 0; CHECK(syscall(SYS_newfstatat, AT_FDCWD, "/", kern, 0) < 0 && errno == EFAULT, "fstatat(kernel buf) errno %d", errno);
    errno = 0; CHECK(syscall(SYS_newfstatat, AT_FDCWD, "/", bad, 0) < 0 && errno == EFAULT, "fstatat(bad buf) errno %d", errno);
    errno = 0; CHECK(syscall(SYS_write, 1, kern, 16) < 0 && errno == EFAULT, "raw write(kernel addr) errno %d", errno);
    CHECK(stat("/", &st) == 0, "stat ok");
    int p[2];
    pipe(p);
    write(p[1], "abcdefgh", 8);
    errno = 0; CHECK(read(p[0], bad, 8) < 0 && errno == EFAULT, "pipe read(bad) errno %d", errno);
    /* tmpfs file: read into a read-only page, a partially unmapped buffer, and fresh demand-zero memory */
    int fd = open("/tmp/hardentest.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    char data[8192];
    memset(data, 'x', sizeof data);
    CHECK(write(fd, data, sizeof data) == sizeof data, "write file");
    char *ro = mmap(0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    errno = 0; CHECK(pread(fd, ro, 16, 0) < 0 && errno == EFAULT, "pread into PROT_READ page errno %d", errno);
    char *two = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(two + 4096, 4096);
    errno = 0;
    long r = pread(fd, two + 4096 - 16, 64, 0);
    CHECK(r == 16 || (r < 0 && errno == EFAULT), "pread across unmapped page: %ld errno %d", r, errno);
    char *fresh = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(pread(fd, fresh + 100, 8000, 0) == 8000 && fresh[100] == 'x' && fresh[8099] == 'x', "pread into fresh pages");
    CHECK(write(fd, fresh + 100, 8000) == 8000, "write from fresh pages");
    /* BKL driver path (tty / devices) with the user-access window */
    int z = open("/dev/zero", O_RDONLY);
    CHECK(read(z, fresh + 20000, 30000) == 30000 && fresh[20000] == 0, "read /dev/zero");
    errno = 0; CHECK(read(z, bad, 100) < 0 && errno == EFAULT, "read /dev/zero into bad errno %d", errno);
    close(z);
    close(fd);
    unlink("/tmp/hardentest.dat");
    long fix1 = hard_val("extable_fixups");
    printf("  extable fixups %ld -> %ld\n", fix0, fix1);
    CHECK(fix1 > fix0, "bad pointers did not go through the exception table");
}

static void wx(void) {
    long w0 = hard_val("wx_mappings");
    void *p = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    long w1 = hard_val("wx_mappings");
    printf("  W+X mmap %s, wx_mappings %ld -> %ld\n", p == MAP_FAILED ? "refused" : "allowed", w0, w1);
    if (p != MAP_FAILED) CHECK(w1 == w0 + 1, "W+X mapping not counted");
    void *q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int r = mprotect(q, 4096, PROT_READ | PROT_EXEC);
    CHECK(r == 0, "mprotect R+X after W");
    CHECK(hard_val("wx_mappings") == w1, "R+X counted as W+X");
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "child")) return child();
    setvbuf(stdout, 0, _IONBF, 0);
    char line[256];
    FILE *f = fopen("/proc/hardening", "r");
    CHECK(f, "/proc/hardening");
    if (f) { while (fgets(line, sizeof line, f)) printf("  %s", line); fclose(f); }
    CHECK(hard_val("kernel_wx_pages") == 0, "kernel W+X pages");
    CHECK(hard_val("stack_guard_random") == 1, "kernel stack guard not randomized");
    CHECK(hard_val("uaccess_violations") == 0, "uaccess violations");
    aslr();
    efault();
    wx();
    printf("hardentest: %s\n", fails ? "FAIL" : "OK");
    return fails != 0;
}
