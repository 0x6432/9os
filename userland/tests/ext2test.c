/* M30 ext2 + unified page cache: file I/O through direct/indirect/double-indirect blocks,
 * sparse files, truncate, directories with many entries, rename/link/symlink/unlink/rmdir,
 * shared mmap + msync, fsync, unlink-while-open, free-space accounting, statfs, and
 * persistence of data and metadata across umount/mount.
 * usage: ext2test DEV MNT   (DEV holds an ext2 filesystem; MNT an empty directory)
 *        ext2test -d DIR    (run in DIR of an already mounted ext2, e.g. the disk root) */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("ext2test: FAIL " __VA_ARGS__); printf(" (errno %d)\n", errno); fails++; } } while (0)
static char base[256];
static char *P(const char *rel) { static char b[4][512]; static int k; k = (k + 1) % 4; snprintf(b[k], sizeof b[k], "%s/%s", base, rel); return b[k]; }

static uint8_t pat(uint64_t off, unsigned seed) { uint64_t x = (off + 1) * 0x9e3779b97f4a7c15ull ^ seed; return (uint8_t)(x >> 31); }
static int write_pat(const char *path, uint64_t off, size_t len, unsigned seed, int flags) {
    int fd = open(path, O_WRONLY | O_CREAT | flags, 0644);
    if (fd < 0) return -1;
    static uint8_t b[64 << 10];
    for (size_t d = 0; d < len;) {
        size_t n = len - d < sizeof b ? len - d : sizeof b;
        for (size_t i = 0; i < n; i++) b[i] = pat(off + d + i, seed);
        if (pwrite(fd, b, n, off + d) != (ssize_t)n) { close(fd); return -1; }
        d += n;
    }
    return close(fd);
}
static int check_pat(const char *path, uint64_t off, size_t len, unsigned seed) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    static uint8_t b[64 << 10];
    for (size_t d = 0; d < len;) {
        size_t n = len - d < sizeof b ? len - d : sizeof b;
        if (pread(fd, b, n, off + d) != (ssize_t)n) { close(fd); return -2; }
        for (size_t i = 0; i < n; i++) if (b[i] != pat(off + d + i, seed)) { close(fd); return -3; }
        d += n;
    }
    close(fd);
    return 0;
}
static uint64_t bfree(void) { struct statfs s; return statfs(base, &s) ? 0 : s.f_bfree; }
static int count_dir(const char *p) {
    DIR *d = opendir(p);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
    closedir(d);
    return n;
}

static void phase1(void) {
    struct statfs sf;
    CHECK(statfs(base, &sf) == 0 && sf.f_type == 0xef53, "statfs type %lx", (long)sf.f_type);
    uint64_t free0 = bfree();
    CHECK(mkdir(P("t"), 0755) == 0, "mkdir t");
    /* small file */
    int fd = open(P("t/hello"), O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && write(fd, "hello ext2\n", 11) == 11, "write hello");
    char buf[64] = { 0 };
    CHECK(pread(fd, buf, sizeof buf, 0) == 11 && !strcmp(buf, "hello ext2\n"), "read hello");
    CHECK(fsync(fd) == 0 && fdatasync(fd) == 0, "fsync");
    close(fd);
    /* 24 MiB: direct, indirect and double-indirect blocks (with 1-4 KiB blocks) */
    CHECK(write_pat(P("t/big"), 0, 24 << 20, 1, O_TRUNC) == 0, "write big");
    CHECK(check_pat(P("t/big"), 0, 24 << 20, 1) == 0, "verify big");
    struct stat st;
    CHECK(stat(P("t/big"), &st) == 0 && st.st_size == 24 << 20, "big size");
    /* overwrite in the middle, unaligned, and append */
    CHECK(write_pat(P("t/big"), 5000001, 70000, 2, 0) == 0, "overwrite");
    CHECK(check_pat(P("t/big"), 5000001, 70000, 2) == 0 && check_pat(P("t/big"), 4990000, 10001, 1) == 0 &&
          check_pat(P("t/big"), 5070001, 9999, 1) == 0, "verify overwrite");
    /* sparse: a hole of 100 MiB costs (almost) no blocks and reads as zeroes */
    uint64_t f1 = bfree();
    fd = open(P("t/sparse"), O_RDWR | O_CREAT, 0644);
    CHECK(fd >= 0 && pwrite(fd, "x", 1, 100 << 20) == 1, "sparse write");
    CHECK(fstat(fd, &st) == 0 && st.st_size == (100 << 20) + 1, "sparse size");
    memset(buf, 1, sizeof buf);
    CHECK(pread(fd, buf, 64, 50 << 20) == 64 && !memchr(buf, 1, 64), "hole reads zero");
    CHECK(pwrite(fd, "y", 1, 12345) == 1 && pread(fd, buf, 3, 12344) == 3 && !buf[0] && buf[1] == 'y' && !buf[2], "write in hole");
    close(fd);
    CHECK(f1 - bfree() < 16, "sparse file used %llu blocks", (unsigned long long)(f1 - bfree()));
    /* truncate down/up: the cut-off tail reads as zeroes when the file grows again */
    CHECK(truncate(P("t/big"), 3000000) == 0 && truncate(P("t/big"), 4000000) == 0, "truncate");
    CHECK(check_pat(P("t/big"), 0, 3000000, 1) == 0, "kept part");
    fd = open(P("t/big"), O_RDONLY);
    static uint8_t z[1000000];
    CHECK(pread(fd, z, sizeof z, 3000000) == sizeof z, "read regrown");
    int nz = 0; for (size_t i = 0; i < sizeof z; i++) nz += z[i] != 0;
    CHECK(nz == 0, "regrown tail not zero (%d)", nz);
    close(fd);
    /* many directory entries (several directory blocks), readdir sees each once */
    CHECK(mkdir(P("t/many"), 0755) == 0, "mkdir many");
    char p[300];
    for (int i = 0; i < 600; i++) {
        snprintf(p, sizeof p, "%s/t/many/file-with-a-longish-name-%04d", base, i);
        int f = open(p, O_CREAT | O_WRONLY, 0600);
        if (f < 0 || write(f, &i, sizeof i) != sizeof i) { fails++; break; }
        close(f);
    }
    CHECK(count_dir(P("t/many")) == 600, "readdir many = %d", count_dir(P("t/many")));
    for (int i = 0; i < 600; i += 2) { snprintf(p, sizeof p, "%s/t/many/file-with-a-longish-name-%04d", base, i); if (unlink(p)) fails++; }
    CHECK(count_dir(P("t/many")) == 300, "after unlink = %d", count_dir(P("t/many")));
    errno = 0;
    CHECK(rmdir(P("t/many")) < 0 && errno == ENOTEMPTY, "rmdir non-empty");
    /* negative dentries must not hide new files */
    CHECK(access(P("t/later"), F_OK) < 0 && errno == ENOENT, "missing");
    CHECK(close(open(P("t/later"), O_CREAT | O_WRONLY, 0644)) == 0 && access(P("t/later"), F_OK) == 0, "created after miss");
    /* rename: same dir, across dirs, over an existing file, directories (.. follows) */
    CHECK(mkdir(P("t/a"), 0755) == 0 && mkdir(P("t/b"), 0755) == 0 && mkdir(P("t/a/sub"), 0700) == 0, "mkdirs");
    CHECK(rename(P("t/later"), P("t/a/moved")) == 0 && access(P("t/later"), F_OK) < 0 && access(P("t/a/moved"), F_OK) == 0, "rename file");
    CHECK(write_pat(P("t/b/victim"), 0, 5000, 3, 0) == 0 && rename(P("t/hello"), P("t/b/victim")) == 0, "rename over");
    fd = open(P("t/b/victim"), O_RDONLY); memset(buf, 0, sizeof buf);
    CHECK(fd >= 0 && read(fd, buf, sizeof buf) == 11 && !strcmp(buf, "hello ext2\n"), "renamed content"); close(fd);
    struct stat sa, sb, ss;
    CHECK(rename(P("t/a/sub"), P("t/b/sub")) == 0, "rename dir");
    CHECK(stat(P("t/b/sub/.."), &ss) == 0 && stat(P("t/b"), &sb) == 0 && ss.st_ino == sb.st_ino, ".. after move");
    CHECK(stat(P("t/a"), &sa) == 0 && sa.st_nlink == 2 && sb.st_nlink == 3, "dir nlinks %d %d", (int)sa.st_nlink, (int)sb.st_nlink);
    errno = 0;
    CHECK(rename(P("t/b"), P("t/b/sub/x")) < 0 && errno == EINVAL, "rename into itself");
    char cwd[512];
    CHECK(chdir(P("t/b/sub")) == 0 && getcwd(cwd, sizeof cwd) && !strcmp(cwd, P("t/b/sub")), "getcwd %s", cwd);
    CHECK(chdir("/") == 0, "chdir /");
    /* links */
    CHECK(link(P("t/b/victim"), P("t/hard")) == 0 && stat(P("t/hard"), &st) == 0 && st.st_nlink == 2, "hard link");
    char longt[200]; memset(longt, 'L', sizeof longt - 1); longt[sizeof longt - 1] = 0; longt[0] = '/';
    CHECK(symlink("b/victim", P("t/fast")) == 0 && symlink(longt, P("t/slow")) == 0, "symlinks");
    memset(buf, 0, sizeof buf);
    CHECK(readlink(P("t/fast"), buf, sizeof buf) == 8 && !memcmp(buf, "b/victim", 8), "readlink fast");
    char lb[256] = { 0 };
    CHECK(readlink(P("t/slow"), lb, sizeof lb) == (ssize_t)strlen(longt) && !strcmp(lb, longt), "readlink slow");
    fd = open(P("t/fast"), O_RDONLY);
    CHECK(fd >= 0, "follow fast symlink"); close(fd);
    CHECK(mkfifo(P("t/fifo"), 0600) == 0 && mknod(P("t/null"), S_IFCHR | 0666, makedev(1, 3)) == 0, "mknod");
    CHECK(chmod(P("t/b/victim"), 0600) == 0 && chown(P("t/b/victim"), 123, 456) == 0, "chmod/chown");
    struct timespec ts[2] = { { 1000000000, 0 }, { 1234567890, 0 } };
    CHECK(utimensat(AT_FDCWD, P("t/b/victim"), ts, 0) == 0, "utimensat");
    /* shared mmap: stores reach the file (msync), and read() sees them at once */
    CHECK(write_pat(P("t/map"), 0, 3 * 4096 + 100, 4, 0) == 0, "map file");
    fd = open(P("t/map"), O_RDWR);
    uint8_t *m = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(m != MAP_FAILED, "mmap");
    if (m != MAP_FAILED) {
        CHECK(m[5000] == pat(5000, 4), "mmap read");
        memcpy(m + 4090, "MAPPED", 6);
        CHECK(pread(fd, buf, 6, 4090) == 6 && !memcmp(buf, "MAPPED", 6), "read sees mmap store");
        CHECK(msync(m, 4 * 4096, MS_SYNC) == 0, "msync");
        munmap(m, 4 * 4096);
    }
    close(fd);
    /* unlink while open: data stays readable; blocks come back after the close */
    CHECK(write_pat(P("t/tmpdel"), 0, 2 << 20, 5, 0) == 0, "tmpdel");
    uint64_t before = bfree();
    fd = open(P("t/tmpdel"), O_RDONLY);
    CHECK(unlink(P("t/tmpdel")) == 0 && access(P("t/tmpdel"), F_OK) < 0, "unlink open file");
    CHECK(pread(fd, z, 4096, 1 << 20) == 4096 && z[7] == pat((1 << 20) + 7, 5), "read unlinked");
    close(fd);
    CHECK(bfree() >= before + 512 - 8, "blocks freed after close: %llu -> %llu", (unsigned long long)before, (unsigned long long)bfree());
    /* concurrent writers to different files (allocator under contention) */
    for (int c = 0; c < 3; c++)
        if (fork() == 0) {
            char q[300]; snprintf(q, sizeof q, "%s/t/par%d", base, c);
            _exit(write_pat(q, 0, 3 << 20, 10 + c, 0) || check_pat(q, 0, 3 << 20, 10 + c));
        }
    int wst, ok = 0;
    while (wait(&wst) > 0) ok += WIFEXITED(wst) && !WEXITSTATUS(wst);
    CHECK(ok == 3, "parallel writers");
    /* a file created and removed leaves the free count as it was */
    uint64_t f2 = bfree();
    CHECK(write_pat(P("t/gone"), 0, 6 << 20, 6, 0) == 0 && unlink(P("t/gone")) == 0, "create+remove");
    CHECK(bfree() == f2, "free blocks %llu != %llu", (unsigned long long)bfree(), (unsigned long long)f2);
    CHECK(free0 > bfree(), "usage accounted");
    sync();
}

/* after umount + mount: everything written above is there */
static void phase2(void) {
    struct stat st;
    CHECK(check_pat(P("t/big"), 0, 3000000, 1) == 0, "big persisted");
    CHECK(stat(P("t/big"), &st) == 0 && st.st_size == 4000000, "big size persisted");
    CHECK(stat(P("t/sparse"), &st) == 0 && st.st_size == (100 << 20) + 1, "sparse persisted");
    CHECK(count_dir(P("t/many")) == 300, "dir persisted");
    CHECK(stat(P("t/b/victim"), &st) == 0 && (st.st_mode & 07777) == 0600 && st.st_uid == 123 && st.st_gid == 456 &&
          st.st_mtime == 1234567890 && st.st_nlink == 2, "metadata persisted (mode %o uid %d mtime %ld nlink %d)",
          st.st_mode, (int)st.st_uid, (long)st.st_mtime, (int)st.st_nlink);
    CHECK(stat(P("t/fifo"), &st) == 0 && S_ISFIFO(st.st_mode), "fifo persisted");
    CHECK(stat(P("t/null"), &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == makedev(1, 3), "chrdev persisted");
    char buf[256] = { 0 };
    CHECK(readlink(P("t/fast"), buf, sizeof buf) == 8, "fast symlink persisted");
    CHECK(readlink(P("t/slow"), buf, sizeof buf) == 199, "slow symlink persisted");
    int fd = open(P("t/map"), O_RDONLY);
    CHECK(fd >= 0 && pread(fd, buf, 6, 4090) == 6 && !memcmp(buf, "MAPPED", 6), "mmap store persisted");
    close(fd);
    for (int c = 0; c < 3; c++) { char q[64]; snprintf(q, sizeof q, "t/par%d", c); CHECK(check_pat(P(q), 0, 3 << 20, 10 + c) == 0, "par%d persisted", c); }
    /* clean up everything */
    char p[300];
    for (int i = 1; i < 600; i += 2) { snprintf(p, sizeof p, "%s/t/many/file-with-a-longish-name-%04d", base, i); if (unlink(p)) fails++; }
    const char *files[] = { "t/many", "t/big", "t/sparse", "t/b/victim", "t/hard", "t/fast", "t/slow", "t/fifo", "t/null", "t/map",
                            "t/a/moved", "t/b/sub", "t/a", "t/b", "t/par0", "t/par1", "t/par2", "t" };
    for (size_t k = 0; k < sizeof files / sizeof *files; k++) {
        int r = unlink(P(files[k]));
        if (r && errno == EISDIR) r = rmdir(P(files[k]));
        CHECK(r == 0, "remove %s", files[k]);
    }
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "-d")) {
        snprintf(base, sizeof base, "%s", argv[2]);
        uint64_t f0 = bfree();
        phase1();
        phase2();
        sync();
        CHECK(bfree() == f0, "free blocks after cleanup %llu != %llu", (unsigned long long)bfree(), (unsigned long long)f0);
    } else if (argc == 3) {
        snprintf(base, sizeof base, "%s", argv[2]);
        mkdir(base, 0755);
        if (mount(argv[1], base, "ext2", 0, NULL)) { printf("ext2test: FAIL mount %s: %s\n", argv[1], strerror(errno)); return 1; }
        uint64_t f0 = bfree();
        phase1();
        errno = 0;
        int fd = open(P("busy"), O_CREAT | O_RDWR, 0644);
        CHECK(umount(base) < 0 && errno == EBUSY, "umount busy");
        close(fd);
        unlink(P("busy"));
        CHECK(umount(base) == 0, "umount");
        CHECK(mount(argv[1], base, "ext2", 0, NULL) == 0, "remount");
        phase2();
        CHECK(bfree() == f0, "free blocks after cleanup %llu != %llu", (unsigned long long)bfree(), (unsigned long long)f0);
        CHECK(umount(base) == 0, "final umount");
    } else { fprintf(stderr, "usage: ext2test DEV MNT | -d DIR\n"); return 2; }
    printf("ext2test: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
