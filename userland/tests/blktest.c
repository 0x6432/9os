/* M29 block layer: raw virtio-blk access through /dev/vdX (buffer cache): ioctls, aligned and
 * unaligned reads/writes, end-of-device behaviour, concurrent writers, fsync, /proc files.
 * usage: blktest [/dev/vdb]   (a scratch disk: its contents are overwritten)
 *        blktest -w DEV SEED / -v DEV SEED: write / verify a pattern (persistence across boots) */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("blktest: FAIL " __VA_ARGS__); putchar('\n'); fails++; } } while (0)
#define BLKGETSIZE64 0x80081272
#define BLKSSZGET 0x1268
#define BLKFLSBUF 0x1261

static uint8_t pat(uint64_t off, unsigned seed) { uint64_t x = (off + 1) * 0x9e3779b97f4a7c15ull ^ seed; return (uint8_t)(x >> 29); }
static void fill(uint8_t *b, size_t n, uint64_t off, unsigned seed) { for (size_t i = 0; i < n; i++) b[i] = pat(off + i, seed); }
static size_t bad(const uint8_t *b, size_t n, uint64_t off, unsigned seed) {
    for (size_t i = 0; i < n; i++) if (b[i] != pat(off + i, seed)) return i + 1;
    return 0;
}

static int persist(const char *dev, unsigned seed, int write_) {
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror(dev); return 1; }
    static uint8_t buf[1 << 20];
    for (int k = 0; k < 4; k++) {
        uint64_t off = (uint64_t)k * 3 << 20;
        if (write_) { fill(buf, sizeof buf, off, seed); CHECK(pwrite(fd, buf, sizeof buf, off) == sizeof buf, "persist write"); }
        else { CHECK(pread(fd, buf, sizeof buf, off) == sizeof buf, "persist read"); CHECK(!bad(buf, sizeof buf, off, seed), "persisted data at %llu", (unsigned long long)off); }
    }
    if (write_) CHECK(fsync(fd) == 0, "fsync");
    close(fd);
    printf("blktest: %s %s\n", write_ ? "wrote" : "verified", fails ? "FAILED" : "ok");
    return fails != 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && (!strcmp(argv[1], "-w") || !strcmp(argv[1], "-v"))) return persist(argv[2], (unsigned)atoi(argv[3]), argv[1][1] == 'w');
    const char *dev = argc > 1 ? argv[1] : "/dev/vdb";
    int fd = open(dev, O_RDWR);
    if (fd < 0) { printf("blktest: FAIL open %s: %s\n", dev, strerror(errno)); return 1; }
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && S_ISBLK(st.st_mode), "not a block device");
    uint64_t size = 0; int ssz = 0;
    CHECK(ioctl(fd, BLKGETSIZE64, &size) == 0 && size >= (8 << 20), "BLKGETSIZE64 %llu", (unsigned long long)size);
    CHECK(ioctl(fd, BLKSSZGET, &ssz) == 0 && ssz == 512, "BLKSSZGET %d", ssz);
    CHECK(lseek(fd, 0, SEEK_END) == (off_t)size, "SEEK_END");

    /* aligned and unaligned writes, then read back through the cache */
    static uint8_t w[1 << 20], r[1 << 20];
    struct { uint64_t off; size_t len; } io[] = {
        { 0, 4096 }, { 4096 * 3, 65536 }, { 1000, 3000 }, { 511, 2 }, { (1 << 20) - 100, 300 },
        { 2 << 20, 1 << 20 }, { size - 512, 512 }, { size - 5000, 5000 },
    };
    for (size_t k = 0; k < sizeof io / sizeof io[0]; k++) {
        fill(w, io[k].len, io[k].off, 1);
        CHECK(pwrite(fd, w, io[k].len, io[k].off) == (ssize_t)io[k].len, "pwrite %zu", k);
    }
    for (size_t k = 0; k < sizeof io / sizeof io[0]; k++) {
        memset(r, 0, io[k].len);
        CHECK(pread(fd, r, io[k].len, io[k].off) == (ssize_t)io[k].len, "pread %zu", k);
        CHECK(!bad(r, io[k].len, io[k].off, 1), "data mismatch %zu at +%zu", k, bad(r, io[k].len, io[k].off, 1) - 1);
    }
    /* end of device */
    CHECK(pread(fd, r, 512, size) == 0, "read at end");
    CHECK(pread(fd, r, 1024, size - 512) == 512, "short read at end");
    errno = 0;
    CHECK(pwrite(fd, w, 512, size) < 0 && errno == ENOSPC, "write past end: %s", strerror(errno));
    CHECK(fsync(fd) == 0, "fsync");
    CHECK(ioctl(fd, BLKFLSBUF, 0) == 0, "BLKFLSBUF");

    /* concurrent writers on disjoint 1 MiB regions, readers verify after */
    for (int c = 0; c < 4; c++) {
        if (fork() == 0) {
            int f = open(dev, O_RDWR);
            static uint8_t b[256 << 10];
            uint64_t base = (uint64_t)(4 + c) << 20;
            for (int k = 0; k < 4; k++) {
                fill(b, sizeof b, base + k * sizeof b, 7 + c);
                if (pwrite(f, b, sizeof b, base + k * sizeof b) != sizeof b) _exit(1);
            }
            _exit(fsync(f) ? 1 : 0);
        }
    }
    int stv, okc = 0;
    while (wait(&stv) > 0) okc += WIFEXITED(stv) && WEXITSTATUS(stv) == 0;
    CHECK(okc == 4, "writers");
    for (int c = 0; c < 4; c++) {
        uint64_t base = (uint64_t)(4 + c) << 20;
        CHECK(pread(fd, r, 1 << 20, base) == 1 << 20 && !bad(r, 1 << 20, base, 7 + c), "concurrent region %d", c);
    }
    /* reopen: same data (one cache per device) */
    close(fd);
    fd = open(dev, O_RDONLY);
    CHECK(pread(fd, r, 65536, 4096 * 3) == 65536 && !bad(r, 65536, 4096 * 3, 1), "after reopen");
    close(fd);

    /* /proc/partitions and /proc/diskstats know the disk */
    char txt[4096] = { 0 };
    int pf = open("/proc/partitions", O_RDONLY);
    CHECK(pf >= 0 && read(pf, txt, sizeof txt - 1) > 0 && strstr(txt, strrchr(dev, '/') + 1), "/proc/partitions");
    close(pf);
    memset(txt, 0, sizeof txt);
    pf = open("/proc/diskstats", O_RDONLY);
    CHECK(pf >= 0 && read(pf, txt, sizeof txt - 1) > 0, "/proc/diskstats");
    close(pf);
    char *l = strstr(txt, strrchr(dev, '/') + 1);
    unsigned long rd = 0, rs = 0, wr = 0, ws = 0, t;
    CHECK(l && sscanf(l, "%*s %lu %lu %lu %lu %lu %lu", &rd, &t, &rs, &t, &wr, &t) == 6 && wr > 0, "diskstats writes %lu", wr);
    (void)ws;
    printf("blktest: %s (%llu MiB, %lu reads, %lu writes)\n", fails ? "FAILED" : "ok", (unsigned long long)(size >> 20), rd, wr);
    return fails != 0;
}
