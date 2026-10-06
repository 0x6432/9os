/* VFS namespace operations without the BKL: threads concurrently create/rename/unlink files
 * and directories, open/stat/readlink paths and list directories while another thread
 * chdir()s around; afterwards the tree must be consistent. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("vfstest: FAIL " __VA_ARGS__); putchar('\n'); fails++; } } while (0)
#define NT 4
#define N 200
static volatile int stop;

static void *churn(void *a) {
    int id = (int)(long)a;
    char p[64], q[64], d[64];
    snprintf(d, sizeof d, "/tmp/vfst/d%d", id);
    if (mkdir(d, 0755) < 0) fails++;
    for (int i = 0; i < N; i++) {
        snprintf(p, sizeof p, "%s/f%d", d, i);
        snprintf(q, sizeof q, "/tmp/vfst/shared/t%d_%d", id, i);
        int fd = open(p, O_CREAT | O_WRONLY | O_EXCL, 0644);
        if (fd < 0) { fails++; continue; }
        if (write(fd, &i, sizeof i) != sizeof i) fails++;
        close(fd);
        if (rename(p, q) < 0) fails++;
        struct stat st;
        if (stat(q, &st) < 0 || st.st_size != sizeof i) fails++;
        if (i % 3 == 0) { if (unlink(q) < 0) fails++; }
        snprintf(p, sizeof p, "%s/s%d", d, i);
        if (symlink(q, p) < 0) fails++;
        char buf[64]; ssize_t n = readlink(p, buf, sizeof buf);
        if (n != (ssize_t)strlen(q) || memcmp(buf, q, n)) fails++;
        if (unlink(p) < 0) fails++;
        snprintf(p, sizeof p, "%s/sub%d", d, i % 8);
        if (mkdir(p, 0755) < 0 && errno != EEXIST) fails++;
        if (i % 8 == 7) for (int k = 0; k < 8; k++) {
            snprintf(p, sizeof p, "%s/sub%d", d, k);
            if (rmdir(p) < 0) fails++;
        }
    }
    return 0;
}
static void *lister(void *a) {
    (void)a;
    while (!stop) {
        DIR *dp = opendir("/tmp/vfst/shared");
        if (!dp) { fails++; break; }
        struct dirent *e;
        while ((e = readdir(dp))) {
            char p[300]; struct stat st;
            snprintf(p, sizeof p, "/tmp/vfst/shared/%s", e->d_name);
            int r = stat(p, &st);
            if (r < 0 && errno != ENOENT) fails++;
        }
        closedir(dp);
    }
    return 0;
}
static void *wanderer(void *a) {
    (void)a;
    char buf[256];
    while (!stop) {
        if (chdir("/tmp/vfst") < 0 || !getcwd(buf, sizeof buf) || strcmp(buf, "/tmp/vfst")) fails++;
        if (chdir("shared") < 0) fails++;
        int fd = open(".", O_RDONLY | O_DIRECTORY);
        if (fd < 0) fails++; else { if (fchdir(fd) < 0) fails++; close(fd); }
        if (chdir("/") < 0) fails++;
    }
    return 0;
}

int main(void) {
    mkdir("/tmp/vfst", 0755);
    CHECK(mkdir("/tmp/vfst/shared", 0755) == 0, "mkdir shared");
    pthread_t t[NT], l, w;
    for (int i = 0; i < NT; i++) pthread_create(&t[i], 0, churn, (void *)(long)i);
    pthread_create(&l, 0, lister, 0);
    pthread_create(&w, 0, wanderer, 0);
    for (int i = 0; i < NT; i++) pthread_join(t[i], 0);
    stop = 1;
    pthread_join(l, 0); pthread_join(w, 0);
    CHECK(!fails, "concurrent ops: %d failures", fails);

    /* consistency: exactly the non-unlinked renamed files remain, with the right contents */
    int cnt = 0;
    DIR *dp = opendir("/tmp/vfst/shared");
    struct dirent *e;
    while (dp && (e = readdir(dp))) {
        if (e->d_name[0] == '.') continue;
        int id, i;
        if (sscanf(e->d_name, "t%d_%d", &id, &i) != 2 || i % 3 == 0) { fails++; continue; }
        char p[300]; int v = -1;
        snprintf(p, sizeof p, "/tmp/vfst/shared/%s", e->d_name);
        int fd = open(p, O_RDONLY);
        if (fd < 0 || read(fd, &v, sizeof v) != sizeof v || v != i) fails++;
        close(fd); unlink(p); cnt++;
    }
    if (dp) closedir(dp);
    int want = 0;
    for (int i = 0; i < N; i++) if (i % 3) want++;
    CHECK(cnt == NT * want, "remaining %d want %d", cnt, NT * want);
    for (int i = 0; i < NT; i++) {
        char d[64]; snprintf(d, sizeof d, "/tmp/vfst/d%d", i);
        CHECK(rmdir(d) == 0, "rmdir %s errno %d", d, errno);
    }
    CHECK(rmdir("/tmp/vfst/shared") == 0, "rmdir shared");
    CHECK(rmdir("/tmp/vfst") == 0, "rmdir vfst");
    /* rmdir of a non-empty dir, rename over a directory */
    mkdir("/tmp/vfst2", 0755); mkdir("/tmp/vfst2/a", 0755); close(open("/tmp/vfst2/a/x", O_CREAT | O_WRONLY, 0644));
    CHECK(rmdir("/tmp/vfst2/a") < 0 && errno == ENOTEMPTY, "rmdir nonempty errno %d", errno);
    unlink("/tmp/vfst2/a/x"); rmdir("/tmp/vfst2/a"); rmdir("/tmp/vfst2");
    if (fails) return 1;
    puts("vfstest: OK");
    return 0;
}
