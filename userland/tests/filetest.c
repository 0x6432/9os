/* tmpfs regular-file I/O without the BKL: threads doing pwrite/pread on disjoint and shared
 * ranges, O_APPEND records never interleave, readv/writev, lseek, mmap(MAP_SHARED) sees
 * write(2) data, truncate while readers run. */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("filetest: FAIL " __VA_ARGS__); putchar('\n'); fails++; } } while (0)
#define NT 4
#define BLK 4096
#define NB 64
static int fd, afd;
static volatile int stop;

static void *writer(void *a) {
    int id = (int)(long)a;
    char buf[BLK];
    for (int r = 0; r < 20; r++)
        for (int b = id; b < NB; b += NT) {
            memset(buf, 'A' + id, BLK);
            buf[0] = (char)r;
            if (pwrite(fd, buf, BLK, (off_t)b * BLK) != BLK) fails++;
        }
    return 0;
}
static void *appender(void *a) {
    int id = (int)(long)a;
    char rec[64];
    for (int i = 0; i < 500; i++) {
        memset(rec, 'a' + id, sizeof rec);
        rec[63] = '\n';
        if (write(afd, rec, sizeof rec) != sizeof rec) fails++;
    }
    return 0;
}
static void *reader(void *a) {
    (void)a;
    char buf[BLK];
    while (!stop) {
        ssize_t n = pread(fd, buf, BLK, 0);
        if (n < 0) fails++;
    }
    return 0;
}

int main(void) {
    fd = open("/tmp/filetest.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    pthread_t t[NT], rd;
    pthread_create(&rd, 0, reader, 0);
    for (int i = 0; i < NT; i++) pthread_create(&t[i], 0, writer, (void *)(long)i);
    for (int i = 0; i < NT; i++) pthread_join(t[i], 0);
    char buf[BLK];
    int bad = 0;
    for (int b = 0; b < NB; b++) {
        if (pread(fd, buf, BLK, (off_t)b * BLK) != BLK) { bad++; continue; }
        char want = 'A' + b % NT;
        if (buf[0] != 19) bad++;
        for (int k = 1; k < BLK; k++) if (buf[k] != want) { bad++; break; }
    }
    CHECK(!bad, "disjoint pwrite blocks: %d bad", bad);
    struct stat st; fstat(fd, &st);
    CHECK(st.st_size == NB * BLK, "size %ld", (long)st.st_size);

    /* truncate while a reader is running, then extend with a hole */
    CHECK(ftruncate(fd, BLK) == 0, "ftruncate");
    stop = 1; pthread_join(rd, 0);
    CHECK(lseek(fd, 0, SEEK_END) == BLK, "lseek end");
    CHECK(pwrite(fd, "z", 1, 3 * BLK) == 1, "write past hole");
    CHECK(pread(fd, buf, 16, 2 * BLK) == 16 && !buf[0] && !buf[15], "hole reads zero");

    /* readv/writev */
    char x[3] = "ab", y[4] = "cde";
    struct iovec wv[2] = { { x, 2 }, { y, 3 } };
    lseek(fd, 0, SEEK_SET);
    CHECK(writev(fd, wv, 2) == 5, "writev");
    char p[2], q[3];
    struct iovec rv[2] = { { p, 2 }, { q, 3 } };
    lseek(fd, 0, SEEK_SET);
    CHECK(readv(fd, rv, 2) == 5 && !memcmp(p, "ab", 2) && !memcmp(q, "cde", 3), "readv");

    /* shared mapping sees write(2) */
    char *m = mmap(0, BLK, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(m != MAP_FAILED, "mmap");
    if (m != MAP_FAILED) {
        pwrite(fd, "MAPPED", 6, 100);
        CHECK(!memcmp(m + 100, "MAPPED", 6), "mmap coherent");
        memcpy(m + 200, "BACK", 4);
        CHECK(pread(fd, buf, 4, 200) == 4 && !memcmp(buf, "BACK", 4), "write through mapping");
        munmap(m, BLK);
    }

    /* O_APPEND: whole records, none lost */
    afd = open("/tmp/filetest.log", O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    for (int i = 0; i < NT; i++) pthread_create(&t[i], 0, appender, (void *)(long)i);
    for (int i = 0; i < NT; i++) pthread_join(t[i], 0);
    close(afd);
    int r = open("/tmp/filetest.log", O_RDONLY);
    char rec[64]; int nrec = 0, torn = 0;
    while (read(r, rec, 64) == 64) {
        nrec++;
        for (int k = 1; k < 63; k++) if (rec[k] != rec[0]) { torn++; break; }
        if (rec[63] != '\n') torn++;
    }
    close(r);
    CHECK(nrec == NT * 500 && !torn, "append records %d torn %d", nrec, torn);
    close(fd);
    unlink("/tmp/filetest.dat"); unlink("/tmp/filetest.log");
    if (fails) return 1;
    puts("filetest: OK");
    return 0;
}
