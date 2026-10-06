/* mapprivtest: MAP_PRIVATE file mappings share page-cache pages until written. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static long vmstat(const char *key) {
    FILE *f = fopen("/proc/vmstat", "r"); char k[64]; long v, r = -1;
    while (f && fscanf(f, "%63s %ld", k, &v) == 2) if (!strcmp(k, key)) r = v;
    if (f) fclose(f);
    return r;
}

int main(void) {
    int fd = open("/tmp/mp.dat", O_CREAT | O_RDWR | O_TRUNC, 0644);
    char buf[3 * 4096 + 100];
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = 'a' + i % 26;
    write(fd, buf, sizeof buf);
    char *m = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(m != MAP_FAILED);
    long before = vmstat("pgfault_file");                            /* demand-faulted from the page cache */
    CHECK(!memcmp(m, buf, sizeof buf));
    CHECK(vmstat("pgfault_file") - before >= 4);
    CHECK(m[sizeof buf] == 0 && m[4 * 4096 - 1] == 0);              /* tail of the last page */
    m[5] = 'X';                                                       /* COW: file unchanged */
    char c; pread(fd, &c, 1, 5);
    CHECK(c == buf[5] && m[5] == 'X');
    char *m2 = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 4096);   /* offset mapping */
    CHECK(m2 != MAP_FAILED && !memcmp(m2, buf + 4096, 4096));
    pwrite(fd, "Z", 1, 4096 + 7);                                     /* unwritten private pages see file writes */
    CHECK(m2[7] == 'Z');
    pid_t p = fork();
    if (p == 0) { m[6] = 'Y'; _exit(m[5] == 'X' ? 0 : 1); }
    int st; waitpid(p, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0 && m[6] == buf[6]);
    CHECK(mprotect(m2, 4096, PROT_READ | PROT_WRITE) == 0);
    m2[0] = 'Q'; pread(fd, &c, 1, 4096);
    CHECK(c == buf[4096]);
    munmap(m, 4 * 4096); munmap(m2, 4096);
    close(fd); unlink("/tmp/mp.dat");
    printf("mapprivtest: %s (%d checks, %d failures)\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails != 0;
}
