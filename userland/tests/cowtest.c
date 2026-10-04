/* cowtest: copy-on-write fork semantics, MAP_SHARED after fork, kernel writes into COW pages. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static long vm(const char *key) {
    FILE *f = fopen("/proc/vmstat", "r"); char k[64]; long v, r = -1;
    if (!f) return -1;
    while (fscanf(f, "%63s %ld", k, &v) == 2) if (!strcmp(k, key)) r = v;
    fclose(f); return r;
}

int main(void) {
    size_t big = 32 << 20;
    char *heap = malloc(big);
    memset(heap, 'p', big);
    static char data[8192] = "parent";
    char *shared = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    strcpy(shared, "before");
    int pfd[2]; pipe(pfd);

    long copied0 = vm("cow_copied");
    double t0 = now();
    pid_t pid = fork();
    if (pid == 0) {
        int bad = 0;
        if (strcmp(data, "parent")) bad |= 1;
        if (heap[12345] != 'p' || heap[big - 1] != 'p') bad |= 2;
        strcpy(data, "child");                       /* must not leak into the parent */
        heap[0] = 'c';
        strcpy(shared, "from child");                /* must be visible to the parent */
        char buf[16] = {0};
        read(pfd[0], heap + 4096, 5);                /* kernel write into a COW page */
        memcpy(buf, heap + 4096, 5);
        if (strcmp(buf, "hello")) bad |= 4;
        _exit(bad);
    }
    double tfork = now() - t0;
    write(pfd[1], "hello", 5);
    int st; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child checks failed (status %#x)", st);
    CHECK(!strcmp(data, "parent"), "child write leaked into parent data: %s", data);
    CHECK(heap[0] == 'p' && heap[4096] == 'p', "child write leaked into parent heap");
    CHECK(!strcmp(shared, "from child"), "MAP_SHARED not shared after fork: %s", shared);
    /* parent writes after the child is gone: page is exclusively ours again */
    heap[0] = 'P'; strcpy(data, "parent2");
    CHECK(heap[0] == 'P' && !strcmp(data, "parent2"), "parent write after child exit");

    /* many forks of a 32 MiB process should be cheap with COW */
    t0 = now();
    for (int i = 0; i < 20; i++) { pid_t c = fork(); if (c == 0) _exit(heap[i * 4096] != 'p' && i); waitpid(c, &st, 0); }
    double t20 = now() - t0;
    long copied = vm("cow_copied") - copied0;
    printf("cowtest: fork of 32 MiB process %.2f ms; 20 fork+exit %.1f ms; pages copied %ld\n",
           tfork * 1e3, t20 * 1e3, copied);
    printf("cowtest: %s\n", fails ? "FAILED" : "PASSED");
    return fails != 0;
}
