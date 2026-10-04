/* musl libc smoke test for 9os. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/utsname.h>

static int fails;
#define CHECK(c) do { if (c) printf("  [ok] %s\n", #c); else { printf("  [FAIL] %s (errno %d)\n", #c, errno); fails++; } } while (0)

static volatile int got_sig;
static void handler(int s) { got_sig = s; }

int main(int argc, char **argv) {
    printf("libctest: argc=%d argv[0]=%s pid=%d\n", argc, argv[0], getpid());
    struct utsname u; uname(&u);
    printf("  uname: %s %s %s\n", u.sysname, u.release, u.machine);

    char *big = malloc(1 << 20);
    CHECK(big != NULL);
    memset(big, 'x', 1 << 20);
    free(big);
    void *m = mmap(0, 8 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED);
    ((char *)m)[(8 << 20) - 1] = 1;
    CHECK(munmap(m, 8 << 20) == 0);

    FILE *f = fopen("/tmp/test.txt", "w");
    CHECK(f != NULL);
    fprintf(f, "line %d\n", 42);
    fclose(f);
    f = fopen("/tmp/test.txt", "r");
    char buf[64] = {0};
    fgets(buf, sizeof buf, f);
    fclose(f);
    CHECK(strcmp(buf, "line 42\n") == 0);
    struct stat st;
    CHECK(stat("/tmp/test.txt", &st) == 0 && st.st_size == 8);
    CHECK(mkdir("/tmp/d", 0755) == 0);
    CHECK(rename("/tmp/test.txt", "/tmp/d/t2") == 0);
    CHECK(symlink("/tmp/d/t2", "/tmp/link") == 0);
    CHECK(readlink("/tmp/link", buf, sizeof buf) == 9);
    DIR *d = opendir("/");
    int n = 0;
    struct dirent *de;
    printf("  /:");
    while ((de = readdir(d))) { printf(" %s", de->d_name); n++; }
    printf("\n");
    closedir(d);
    CHECK(n >= 4);
    CHECK(chdir("/tmp/d") == 0);
    CHECK(getcwd(buf, sizeof buf) && strcmp(buf, "/tmp/d") == 0);

    int p[2];
    CHECK(pipe(p) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        write(p[1], "through pipe", 12);
        _exit(7);
    }
    close(p[1]);
    memset(buf, 0, sizeof buf);
    CHECK(read(p[0], buf, sizeof buf) == 12);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 7);

    signal(SIGUSR1, handler);
    kill(getpid(), SIGUSR1);
    CHECK(got_sig == SIGUSR1);

    pid = fork();
    if (pid == 0) { pause(); _exit(0); }
    kill(pid, SIGTERM);
    CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    usleep(50000);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CHECK(ms >= 50 && ms < 200);

    printf("libctest: %s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    if (getpid() == 1) for (;;) pause();
    return fails;
}
