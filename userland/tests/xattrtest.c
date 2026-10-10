/* Extended attributes (M33) on the filesystem holding DIR (default /tmp: tmpfs; ext2 in the
 * disk-root and /mnt runs): set/get/list/remove through path, l* and f* calls, XATTR_CREATE/
 * REPLACE, size probing and ERANGE, namespaces (user/trusted/security, EOPNOTSUPP for others),
 * user.* refused on symlinks/devices, permission checks as an unprivileged user, many and large
 * attributes (ENOSPC/E2BIG limits), persistence across a drop of the inode cache (-r: re-read
 * attributes written by an earlier run), and removal of the last attribute. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("xattrtest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" (errno %d)\n", errno); fails++; } } while (0)

static int has_name(const char *list, ssize_t n, const char *name) {
    for (ssize_t p = 0; p < n; p += strlen(list + p) + 1) if (!strcmp(list + p, name)) return 1;
    return 0;
}

static void test_basic(const char *dir) {
    char f[256], l[256], d[256];
    snprintf(f, sizeof f, "%s/xa_file", dir);
    snprintf(l, sizeof l, "%s/xa_link", dir);
    snprintf(d, sizeof d, "%s/xa_dir", dir);
    unlink(f); unlink(l); rmdir(d);
    int fd = open(f, O_RDWR | O_CREAT, 0644);
    CHECK(fd >= 0, "create %s", f);
    char buf[512];
    errno = 0;
    CHECK(getxattr(f, "user.none", buf, sizeof buf) < 0 && errno == ENODATA, "missing attr");
    CHECK(listxattr(f, buf, sizeof buf) == 0, "empty list");
    CHECK(setxattr(f, "user.color", "blue", 4, 0) == 0, "set user.color");
    CHECK(getxattr(f, "user.color", NULL, 0) == 4, "size probe");
    memset(buf, 0, sizeof buf);
    CHECK(getxattr(f, "user.color", buf, sizeof buf) == 4 && !memcmp(buf, "blue", 4), "get %s", buf);
    CHECK(getxattr(f, "user.color", buf, 2) < 0 && errno == ERANGE, "ERANGE");
    CHECK(setxattr(f, "user.color", "red", 3, XATTR_CREATE) < 0 && errno == EEXIST, "XATTR_CREATE on existing");
    CHECK(setxattr(f, "user.shape", "x", 1, XATTR_REPLACE) < 0 && errno == ENODATA, "XATTR_REPLACE missing");
    CHECK(fsetxattr(fd, "user.color", "red", 3, XATTR_REPLACE) == 0, "fsetxattr replace");
    CHECK(fgetxattr(fd, "user.color", buf, sizeof buf) == 3 && !memcmp(buf, "red", 3), "fget");
    CHECK(setxattr(f, "user.empty", "", 0, 0) == 0, "empty value");
    CHECK(getxattr(f, "user.empty", buf, sizeof buf) == 0, "get empty");
    CHECK(setxattr(f, "trusted.t", "tt", 2, 0) == 0, "trusted as root");
    CHECK(setxattr(f, "security.s", "ss", 2, 0) == 0, "security as root");
    CHECK(setxattr(f, "system.foo", "x", 1, 0) < 0 && errno == EOPNOTSUPP, "system.* unsupported");
    CHECK(setxattr(f, "bogus.foo", "x", 1, 0) < 0 && errno == EOPNOTSUPP, "unknown ns");
    CHECK(setxattr(f, "user.", "x", 1, 0) < 0, "empty suffix");
    CHECK(setxattr(f, "", "x", 1, 0) < 0 && errno == ERANGE, "empty name");
    char longname[300]; memset(longname, 'a', sizeof longname); memcpy(longname, "user.", 5); longname[299] = 0;
    CHECK(setxattr(f, longname, "x", 1, 0) < 0 && errno == ERANGE, "name too long");
    CHECK(setxattr(f, "user.big", buf, 70000, 0) < 0 && errno == E2BIG, "E2BIG");
    CHECK(setxattr(f, "user.c", "x", 1, 0x10) < 0 && errno == EINVAL, "bad flags");
    ssize_t need = listxattr(f, NULL, 0);
    ssize_t n = listxattr(f, buf, sizeof buf);
    CHECK(n == need && n > 0, "list size %zd vs %zd", n, need);
    CHECK(has_name(buf, n, "user.color") && has_name(buf, n, "user.empty") && has_name(buf, n, "trusted.t") && has_name(buf, n, "security.s"),
          "list contents");
    CHECK(listxattr(f, buf, 3) < 0 && errno == ERANGE, "list ERANGE");
    CHECK(flistxattr(fd, buf, sizeof buf) == n, "flistxattr");
    /* symlinks: l* calls act on the link; user.* not allowed there */
    CHECK(symlink("xa_file", l) == 0, "symlink");
    CHECK(getxattr(l, "user.color", buf, sizeof buf) == 3, "follow link");
    CHECK(lsetxattr(l, "user.color", "x", 1, 0) < 0 && errno == EPERM, "user.* on symlink");
    CHECK(lgetxattr(l, "user.color", buf, sizeof buf) < 0 && errno == ENODATA, "lget on link");
    CHECK(lsetxattr(l, "trusted.lnk", "L", 1, 0) == 0, "trusted on symlink");
    CHECK(llistxattr(l, buf, sizeof buf) == (ssize_t)sizeof "trusted.lnk" && !strcmp(buf, "trusted.lnk"), "llist");
    CHECK(lremovexattr(l, "trusted.lnk") == 0, "lremove");
    /* directories */
    CHECK(mkdir(d, 0755) == 0, "mkdir");
    CHECK(setxattr(d, "user.dir", "D", 1, 0) == 0, "dir attr");
    CHECK(getxattr(d, "user.dir", buf, sizeof buf) == 1, "get dir attr");
    /* unprivileged user: DAC on user.*, trusted.* hidden and refused, security.* readable */
    pid_t p = fork();
    if (!p) {
        if (setgid(1000) || setuid(1000)) _exit(90);
        int bad = 0;
        if (getxattr(f, "user.color", buf, sizeof buf) != 3) bad |= 1;                       /* 0644: readable */
        if (!(setxattr(f, "user.color", "z", 1, 0) < 0 && errno == EACCES)) bad |= 2;       /* not writable */
        if (!(getxattr(f, "trusted.t", buf, sizeof buf) < 0 && errno == EPERM)) bad |= 4;
        if (getxattr(f, "security.s", buf, sizeof buf) != 2) bad |= 8;
        if (!(setxattr(f, "security.s", "q", 1, 0) < 0 && errno == EPERM)) bad |= 16;
        ssize_t m = listxattr(f, buf, sizeof buf);
        if (m <= 0 || has_name(buf, m, "trusted.t") || !has_name(buf, m, "user.color")) bad |= 32;
        _exit(bad);
    }
    int st; waitpid(p, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "unprivileged checks: %#x", WEXITSTATUS(st));
    /* remove */
    CHECK(removexattr(f, "user.empty") == 0, "remove");
    CHECK(removexattr(f, "user.empty") < 0 && errno == ENODATA, "remove missing");
    CHECK(fremovexattr(fd, "trusted.t") == 0 && removexattr(f, "security.s") == 0, "fremove");
    n = listxattr(f, buf, sizeof buf);
    CHECK(n == (ssize_t)sizeof "user.color" && !strcmp(buf, "user.color"), "list after remove (%zd)", n);
    CHECK(removexattr(f, "user.color") == 0 && listxattr(f, buf, sizeof buf) == 0, "last attribute removed");
    close(fd);
    CHECK(unlink(l) == 0 && rmdir(d) == 0, "cleanup");
}

/* many attributes until the filesystem refuses (ext2: one block), then remove all */
static void test_many(const char *dir) {
    char f[256], name[64], val[100];
    snprintf(f, sizeof f, "%s/xa_many", dir);
    int fd = open(f, O_RDWR | O_CREAT | O_TRUNC, 0644);
    close(fd);
    int k;
    for (k = 0; k < 200; k++) {
        snprintf(name, sizeof name, "user.attr%03d", k);
        memset(val, 'a' + k % 26, sizeof val);
        if (setxattr(f, name, val, 1 + k % 90, 0)) break;
    }
    CHECK(k == 200 || errno == ENOSPC, "stopped at %d with errno %d", k, errno);
    int nset = k;
    for (k = 0; k < nset; k++) {
        char got[100];
        snprintf(name, sizeof name, "user.attr%03d", k);
        ssize_t n = getxattr(f, name, got, sizeof got);
        if (n != 1 + k % 90 || got[0] != 'a' + k % 26 || got[n - 1] != 'a' + k % 26) { CHECK(0, "readback %d: %zd", k, n); break; }
    }
    /* a value larger than an ext2 block is refused; 4000 bytes may or may not fit */
    char *big = malloc(65536); memset(big, 'B', 65536);
    int r = setxattr(f, "user.huge", big, 65536, 0);
    CHECK(r == 0 || errno == ENOSPC || errno == E2BIG, "huge value");
    if (!r) CHECK(getxattr(f, "user.huge", big, 65536) == 65536, "huge readback");
    removexattr(f, "user.huge");
    for (k = 0; k < nset; k++) { snprintf(name, sizeof name, "user.attr%03d", k); if (removexattr(f, name)) { CHECK(0, "remove %d", k); break; } }
    CHECK(listxattr(f, NULL, 0) == 0, "all removed");
    /* attributes go away with the file */
    setxattr(f, "user.gone", "1", 1, 0);
    CHECK(unlink(f) == 0, "unlink");
    free(big);
    printf("xattrtest: %d attributes in %s\n", nset, dir);
}

/* persistence: -w writes, -r (after a reboot) verifies and cleans up */
static void persist(const char *dir, int write_) {
    char f[256], buf[64];
    snprintf(f, sizeof f, "%s/xa_persist", dir);
    if (write_) {
        int fd = open(f, O_RDWR | O_CREAT | O_TRUNC, 0644);
        close(fd);
        CHECK(setxattr(f, "user.persist", "kept-value", 10, 0) == 0, "set persist");
        CHECK(setxattr(f, "trusted.persist", "T", 1, 0) == 0, "set trusted persist");
        sync();
    } else {
        CHECK(getxattr(f, "user.persist", buf, sizeof buf) == 10 && !memcmp(buf, "kept-value", 10), "persisted value");
        CHECK(getxattr(f, "trusted.persist", buf, sizeof buf) == 1, "persisted trusted");
        CHECK(unlink(f) == 0, "unlink persisted");
    }
}

int main(int argc, char **argv) {
    const char *dir = "/tmp";
    int mode = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w")) mode = 1;
        else if (!strcmp(argv[i], "-r")) mode = 2;
        else dir = argv[i];
    }
    if (mode) persist(dir, mode == 1);
    else { test_basic(dir); test_many(dir); }
    if (fails) { printf("xattrtest: %d failures\n", fails); return 1; }
    printf("xattrtest: all passed\n");
    return 0;
}
