/* VMM v2 (M26): mremap, madvise, mlock/mlockall, msync, mincore, MAP_SHARED anonymous memory
 * across fork, shared file mappings, MAP_FIXED_NOREPLACE, stack guard gap, many VMAs, and
 * memory pressure: a hog is OOM-killed while page-cache pages are reclaimed and refaulted
 * (binaries still checksum the same and still run). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("vmtest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); putchar('\n'); fails++; } } while (0)
#define PG 4096UL
#ifndef MADV_POPULATE_READ
#define MADV_POPULATE_READ 22
#define MADV_POPULATE_WRITE 23
#endif
#ifndef MREMAP_DONTUNMAP
#define MREMAP_DONTUNMAP 4
#endif
#ifndef MLOCK_ONFAULT
#define MLOCK_ONFAULT 1
#endif

static long status_kb(const char *key) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256]; long v = -1;
    size_t kl = strlen(key);
    while (f && fgets(line, sizeof line, f)) if (!strncmp(line, key, kl)) v = strtol(line + kl, 0, 10);
    if (f) fclose(f);
    return v;
}
static long vmstat(const char *key) {
    FILE *f = fopen("/proc/vmstat", "r");
    char line[256]; long v = -1;
    size_t kl = strlen(key);
    while (f && fgets(line, sizeof line, f)) if (!strncmp(line, key, kl) && line[kl] == ' ') v = strtol(line + kl, 0, 10);
    if (f) fclose(f);
    return v;
}
static uint32_t file_sum(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    static unsigned char buf[65536];
    uint32_t h = 2166136261u;
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) for (ssize_t i = 0; i < n; i++) h = (h ^ buf[i]) * 16777619u;
    close(fd);
    return h;
}
static int run(const char *cmd) {
    pid_t p = fork();
    if (!p) { execl("/bin/sh", "sh", "-c", cmd, (char *)0); _exit(127); }
    int st; waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static void test_mremap(void) {
    char *a = mmap(0, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (int i = 0; i < 4; i++) memset(a + i * PG, 'a' + i, PG);
    /* move: force by blocking the space after it */
    char *blk = mmap(a + 4 * PG, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    char *b = mremap(a, 4 * PG, 64 * PG, 0);
    CHECK(b == MAP_FAILED && errno == ENOMEM, "mremap without MAYMOVE should fail when blocked");
    b = mremap(a, 4 * PG, 64 * PG, MREMAP_MAYMOVE);
    CHECK(b != MAP_FAILED && b != a, "mremap move %p", b);
    for (int i = 0; i < 4; i++) CHECK(b[i * PG] == 'a' + i && b[i * PG + PG - 1] == 'a' + i, "moved data page %d", i);
    CHECK(b[10 * PG] == 0, "grown part is zero");
    b[63 * PG] = 'z';
    /* shrink in place */
    char *c = mremap(b, 64 * PG, 2 * PG, 0);
    CHECK(c == b && b[PG] == 'b', "shrink");
    unsigned char v[2];
    CHECK(mincore(b + 2 * PG, PG, v) == -1 && errno == ENOMEM, "shrunk tail unmapped");
    /* grow in place */
    munmap(b + 2 * PG, 62 * PG);
    c = mremap(b, 2 * PG, 8 * PG, 0);
    CHECK(c == b && b[0] == 'a', "grow in place %p %p", c, b);
    /* MREMAP_FIXED */
    char *dst = mmap(0, 8 * PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    c = mremap(b, 8 * PG, 8 * PG, MREMAP_MAYMOVE | MREMAP_FIXED, dst);
    CHECK(c == dst && dst[0] == 'a' && dst[PG] == 'b', "MREMAP_FIXED");
    /* MREMAP_DONTUNMAP: source stays mapped, refaults zero */
    char *e = mremap(dst, 8 * PG, 8 * PG, MREMAP_MAYMOVE | MREMAP_DONTUNMAP);
    CHECK(e != MAP_FAILED && e[0] == 'a' && dst[0] == 0, "DONTUNMAP %p", e);
    CHECK(mremap(dst, PG, 2 * PG, MREMAP_FIXED, dst + 100 * PG) == MAP_FAILED && errno == EINVAL, "FIXED needs MAYMOVE");
    munmap(dst, 8 * PG); munmap(e, 8 * PG); munmap(blk, PG);
}

static void test_madvise(void) {
    char *a = mmap(0, 16 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(a, 7, 16 * PG);
    CHECK(madvise(a, 8 * PG, MADV_DONTNEED) == 0, "DONTNEED");
    CHECK(a[0] == 0 && a[8 * PG - 1] == 0 && a[8 * PG] == 7, "DONTNEED zeroes only the range");
    CHECK(madvise(a + 8 * PG, 8 * PG, MADV_FREE) == 0, "FREE");
    CHECK(madvise(a, 16 * PG, MADV_WILLNEED) == 0 && madvise(a, 16 * PG, MADV_POPULATE_WRITE) == 0, "WILLNEED/POPULATE");
    unsigned char v[16];
    CHECK(mincore(a, 16 * PG, v) == 0 && v[0] && v[15], "populated");
    CHECK(madvise(a + 1, PG, MADV_DONTNEED) == -1 && errno == EINVAL, "unaligned");
    CHECK(madvise(a, PG, 12345) == -1 && errno == EINVAL, "bad advice");
    munmap(a + 4 * PG, PG);
    CHECK(madvise(a, 16 * PG, MADV_NORMAL) == -1 && errno == ENOMEM, "hole -> ENOMEM");
    munmap(a, 16 * PG);
    /* private file mapping: DONTNEED drops the private copy, the file data comes back */
    int fd = open("/tmp/vmtest.f", O_RDWR | O_CREAT | O_TRUNC, 0600);
    char buf[2 * PG]; memset(buf, 'F', sizeof buf);
    write(fd, buf, sizeof buf);
    char *m = mmap(0, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    m[0] = 'P';
    char r1; pread(fd, &r1, 1, 0);
    CHECK(m[0] == 'P' && r1 == 'F', "private write not visible in file");
    madvise(m, 2 * PG, MADV_DONTNEED);
    CHECK(m[0] == 'F', "DONTNEED on private file mapping restores file data (%c)", m[0]);
    munmap(m, 2 * PG);
    /* shared file mapping coherent with read/write */
    char *s = mmap(0, 2 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    s[5] = 'S';
    pread(fd, &r1, 1, 5);
    CHECK(r1 == 'S', "shared store visible via read");
    pwrite(fd, "W", 1, PG + 1);
    CHECK(s[PG + 1] == 'W', "write visible via shared mapping");
    CHECK(msync(s, 2 * PG, MS_SYNC) == 0 && msync(s, PG, MS_ASYNC) == 0, "msync");
    CHECK(msync(s, PG, MS_SYNC | MS_ASYNC) == -1 && errno == EINVAL, "msync flags");
    CHECK(msync(s + 1, PG, MS_SYNC) == -1 && errno == EINVAL, "msync unaligned");
    munmap(s, 2 * PG);
    CHECK(msync(s, PG, MS_SYNC) == -1 && errno == ENOMEM, "msync unmapped");
    /* pages past EOF of a private mapping read as zero */
    char *z = mmap(0, 4 * PG, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(z[3 * PG] == 0 && z[5] == 'S', "past-EOF page");
    munmap(z, 4 * PG);
    close(fd);
    unlink("/tmp/vmtest.f");
}

static void test_mlock(void) {
    long l0 = status_kb("VmLck:");
    char *a = mmap(0, 32 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(mlock(a, 32 * PG) == 0, "mlock");
    CHECK(status_kb("VmLck:") == l0 + 128, "VmLck %ld -> %ld", l0, status_kb("VmLck:"));
    unsigned char v[32];
    CHECK(mincore(a, 32 * PG, v) == 0 && v[0] && v[31], "mlock populates");
    CHECK(madvise(a, PG, MADV_DONTNEED) == -1 && errno == EINVAL, "DONTNEED on locked");
    CHECK(munlock(a, 16 * PG) == 0 && status_kb("VmLck:") == l0 + 64, "munlock half");
    CHECK(mlock2(a, 4 * PG, MLOCK_ONFAULT) == 0, "mlock2 ONFAULT");
    CHECK(munlock(a, 32 * PG) == 0 && status_kb("VmLck:") == l0, "munlock all");
    munmap(a, 32 * PG);
    CHECK(mlock(a, PG) == -1 && errno == ENOMEM, "mlock unmapped");
    CHECK(mlockall(MCL_CURRENT) == 0 && status_kb("VmLck:") > 0, "mlockall");
    CHECK(munlockall() == 0 && status_kb("VmLck:") == 0, "munlockall");
    CHECK(mlockall(0) == -1 && errno == EINVAL, "mlockall(0)");
    CHECK(mlockall(MCL_FUTURE) == 0, "MCL_FUTURE");
    char *b = mmap(0, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(mincore(b, 4 * PG, v) == 0 && v[3], "MCL_FUTURE populates new mappings");
    munlockall();
    munmap(b, 4 * PG);
}

static void test_shared_fork(void) {
    volatile int *sh = mmap(0, 2 * PG, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(sh != MAP_FAILED, "shared anon");
    int *pr = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    sh[0] = 1; pr[0] = 1;
    pid_t p = fork();
    if (!p) { sh[0] = 42; sh[PG / sizeof(int)] = 43; pr[0] = 99; _exit(0); }
    int st; waitpid(p, &st, 0);
    CHECK(sh[0] == 42 && sh[PG / sizeof(int)] == 43, "child store visible in shared anon (%d)", sh[0]);
    CHECK(pr[0] == 1, "private stays private");
    int zfd = open("/dev/zero", O_RDWR);
    volatile int *z = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_SHARED, zfd, 0);
    close(zfd);
    p = fork();
    if (!p) { z[1] = 7; _exit(0); }
    waitpid(p, &st, 0);
    CHECK(z[1] == 7, "MAP_SHARED /dev/zero shared across fork");
    munmap((void *)sh, 2 * PG); munmap(pr, PG); munmap((void *)z, PG);
}

static void test_layout(void) {
    char *a = mmap(0, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(mmap(a, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED && errno == EEXIST,
          "FIXED_NOREPLACE");
    CHECK(mmap(0, PG, PROT_READ, MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EINVAL, "no MAP_PRIVATE/SHARED");
    int ro = open("/bin/busybox", O_RDONLY);
    CHECK(mmap(0, PG, PROT_READ | PROT_WRITE, MAP_SHARED, ro, 0) == MAP_FAILED && errno == EACCES, "shared RW of O_RDONLY");
    close(ro);
    munmap(a, PG);
    /* many VMAs: alternate protections, then the tree must still find and free everything */
    enum { N = 1500 };
    char *big = mmap(0, N * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (int i = 0; i < N; i += 2) mprotect(big + i * PG, PG, PROT_READ);
    for (int i = 0; i < N; i++) if (i & 1) big[i * PG] = (char)i;
    int ok = 1;
    for (int i = 1; i < N; i += 2) if (big[i * PG] != (char)i) ok = 0;
    CHECK(ok, "data in split VMAs");
    for (int i = 0; i < N; i += 2) mprotect(big + i * PG, PG, PROT_READ | PROT_WRITE);   /* merges back */
    FILE *f = fopen("/proc/self/maps", "r");
    char line[256]; int n = 0;
    unsigned long sbot = 0, lo, hi;
    while (fgets(line, sizeof line, f)) { n++; if (strstr(line, "[stack]") && sscanf(line, "%lx-%lx", &lo, &hi) == 2) sbot = lo; }
    fclose(f);
    CHECK(n < 40, "VMAs merged again (%d)", n);
    munmap(big, N * PG);
    /* guard gap: fill the space below the stack with hint mappings; none may land in the gap */
    CHECK(sbot != 0, "stack found");
    int bad = 0;
    for (int i = 0; i < 64; i++) {
        char *h = mmap((void *)(sbot - PG * (i + 1)), PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if ((unsigned long)h + PG > sbot - (1UL << 20) && (unsigned long)h < sbot && (unsigned long)h != sbot - PG * (i + 1)) bad++;
        munmap(h, PG);
    }
    CHECK(!bad, "non-hint placement inside the stack guard gap");
}

static void test_pressure(void) {
    uint32_t before = file_sum("/bin/busybox"), bash_before = file_sum("/bin/bash");
    long rec0 = vmstat("pagecache_reclaimed"), oom0 = vmstat("oom_kill");
    pid_t p = fork();
    if (!p) {
        /* hog: touch anonymous memory until the OOM killer stops us */
        for (;;) {
            char *m = mmap(0, 4 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (m == MAP_FAILED) continue;
            for (size_t o = 0; o < (4 << 20); o += PG) m[o] = 1;
        }
    }
    int st; waitpid(p, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "hog killed by SIGKILL (status %x)", st);
    CHECK(vmstat("oom_kill") > oom0, "oom_kill counter");
    long rec = vmstat("pagecache_reclaimed");
    CHECK(rec > rec0, "page cache reclaimed under pressure (%ld -> %ld)", rec0, rec);
    CHECK(file_sum("/bin/busybox") == before && file_sum("/bin/bash") == bash_before, "binaries intact after reclaim");
    CHECK(run("bash -c 'x=$((6*7)); [ $x = 42 ]' && echo ok | grep -q ok") == 0, "programs still run after reclaim");
    printf("vmtest: pressure: reclaimed %ld pages, oom kills %ld\n", rec - rec0, vmstat("oom_kill") - oom0);
}

int main(int argc, char **argv) {
    test_mremap();
    test_madvise();
    test_mlock();
    test_shared_fork();
    test_layout();
    if (argc < 2 || strcmp(argv[1], "nopressure")) test_pressure();
    if (fails) { printf("vmtest: %d failures\n", fails); return 1; }
    puts("vmtest: OK");
    return 0;
}
