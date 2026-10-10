/* POSIX ACLs (M33) on the filesystem holding DIR (default /tmp: tmpfs; ext2 in the /mnt and
 * disk-root runs), driven through raw system.posix_acl_* xattrs (v2 format): named-user and
 * named-group grants with the mask, chmod updating the mask, mode-equivalent ACLs collapsing
 * into the mode, validation (EINVAL/EOPNOTSUPP/EACCES) and owner-only writes (EPERM),
 * default-ACL inheritance on create/mkdir (the umask is ignored), and persistence across a
 * reboot (-w writes, -r re-checks). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("acltest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" (errno %d)\n", errno); fails++; } } while (0)

enum { UO = 1, U = 2, GO = 4, G = 8, M = 0x10, O = 0x20 };
#define ACC "system.posix_acl_access"
#define DEF "system.posix_acl_default"
struct ent { uint16_t tag, perm; uint32_t id; };

static size_t blob(uint8_t *b, const struct ent *e, int n) {
    uint32_t v = 2; memcpy(b, &v, 4);
    for (int k = 0; k < n; k++) memcpy(b + 4 + 8 * k, &e[k], 8);
    return 4 + 8 * (size_t)n;
}
static int setacl(const char *p, const char *name, const struct ent *e, int n) {
    uint8_t b[256];
    return setxattr(p, name, b, blob(b, e, n), 0);
}
/* perm of entry tag/id in the stored ACL, -1 if absent, -2 if no ACL */
static int aclperm(const char *p, const char *name, int tag, uint32_t id) {
    uint8_t b[256];
    ssize_t n = getxattr(p, name, b, sizeof b);
    if (n < 4) return -2;
    for (ssize_t o = 4; o + 8 <= n; o += 8) {
        struct ent e; memcpy(&e, b + o, 8);
        if (e.tag == tag && (tag != U && tag != G ? 1 : e.id == id)) return e.perm;
    }
    return -1;
}
static unsigned fmode(const char *p) { struct stat st; return stat(p, &st) ? 0xffff : st.st_mode & 07777; }

/* open p with flags as uid/gid (+ one supplementary group if sg): 0 or errno */
static int try_as(const char *p, int flags, uid_t uid, gid_t gid, gid_t sg) {
    pid_t pid = fork();
    if (!pid) {
        gid_t g[1] = { sg };
        if (setgroups(sg ? 1 : 0, g) || setgid(gid) || setuid(uid)) _exit(99);
        int fd = open(p, flags);
        _exit(fd >= 0 ? 0 : errno);
    }
    int st; waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 98;
}

static void test_access(const char *dir) {
    char f[256]; snprintf(f, sizeof f, "%s/acl_f", dir);
    unlink(f);
    int fd = open(f, O_CREAT | O_RDWR, 0640); close(fd);
    chmod(f, 0640);
    errno = 0;
    CHECK(getxattr(f, ACC, NULL, 0) < 0 && errno == ENODATA, "no ACL initially");
    CHECK(try_as(f, O_RDONLY, 1000, 1000, 0) == EACCES, "other denied before ACL");
    struct ent a[] = { {UO, 6, 0}, {U, 4, 1000}, {GO, 4, 0}, {G, 6, 2000}, {M, 6, 0}, {O, 0, 0} };
    CHECK(setacl(f, ACC, a, 6) == 0, "set access ACL");
    CHECK(fmode(f) == 0660, "mode mirrors mask: %o", fmode(f));
    char lb[512]; ssize_t ln = listxattr(f, lb, sizeof lb);
    int listed = 0;
    for (ssize_t o = 0; o < ln; o += strlen(lb + o) + 1) if (!strcmp(lb + o, ACC)) listed = 1;
    CHECK(listed, "ACL listed");
    CHECK(try_as(f, O_RDONLY, 1000, 1000, 0) == 0, "named user reads");
    CHECK(try_as(f, O_WRONLY, 1000, 1000, 0) == EACCES, "named user can't write");
    CHECK(try_as(f, O_RDWR, 1001, 1001, 2000) == 0, "named group rw");
    CHECK(try_as(f, O_RDWR, 1001, 0, 0) == EACCES, "owning group: r only");
    CHECK(try_as(f, O_RDONLY, 1001, 0, 0) == 0, "owning group reads");
    CHECK(try_as(f, O_RDONLY, 1002, 1002, 0) == EACCES, "other denied");
    CHECK(chmod(f, 0600) == 0 && aclperm(f, ACC, M, 0) == 0, "chmod clears mask");
    CHECK(try_as(f, O_RDONLY, 1000, 1000, 0) == EACCES, "mask blocks named user");
    CHECK(try_as(f, O_RDONLY, 1001, 1001, 2000) == EACCES, "mask blocks named group");
    CHECK(chmod(f, 0644) == 0 && aclperm(f, ACC, M, 0) == 4 && aclperm(f, ACC, O, 0) == 4, "chmod sets mask/other");
    CHECK(aclperm(f, ACC, U, 1000) == 4 && aclperm(f, ACC, G, 2000) == 6, "named entries kept");
    CHECK(try_as(f, O_RDONLY, 1000, 1000, 0) == 0, "named user again");
    CHECK(try_as(f, O_WRONLY, 1001, 1001, 2000) == EACCES, "mask r limits group write");
    /* only the owner (or CAP_FOWNER) changes ACLs; anyone reads them */
    pid_t pid = fork();
    if (!pid) {
        if (setgid(1000) || setuid(1000)) _exit(99);
        uint8_t b[256];
        if (getxattr(f, ACC, b, sizeof b) <= 0) _exit(1);
        if (setacl(f, ACC, a, 6) == 0 || errno != EPERM) _exit(2);
        if (removexattr(f, ACC) == 0 || errno != EPERM) _exit(3);
        _exit(0);
    }
    int st; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "non-owner: status %x", st);
    /* validation */
    struct ent nomask[] = { {UO, 6, 0}, {U, 4, 1000}, {GO, 4, 0}, {O, 0, 0} };
    CHECK(setacl(f, ACC, nomask, 4) < 0 && errno == EINVAL, "named without mask");
    struct ent unsorted[] = { {UO, 6, 0}, {U, 4, 1001}, {U, 4, 1000}, {GO, 4, 0}, {M, 4, 0}, {O, 0, 0} };
    CHECK(setacl(f, ACC, unsorted, 6) < 0 && errno == EINVAL, "unsorted");
    struct ent dup[] = { {UO, 6, 0}, {UO, 6, 0}, {GO, 4, 0}, {O, 0, 0} };
    CHECK(setacl(f, ACC, dup, 4) < 0 && errno == EINVAL, "duplicate USER_OBJ");
    struct ent badperm[] = { {UO, 8, 0}, {GO, 4, 0}, {O, 0, 0} };
    CHECK(setacl(f, ACC, badperm, 3) < 0 && errno == EINVAL, "perm > 7");
    struct ent noother[] = { {UO, 6, 0}, {GO, 4, 0} };
    CHECK(setacl(f, ACC, noother, 2) < 0 && errno == EINVAL, "missing OTHER");
    uint8_t b[64]; size_t bl = blob(b, a, 3); uint32_t v1 = 1; memcpy(b, &v1, 4);
    CHECK(setxattr(f, ACC, b, bl, 0) < 0 && errno == EOPNOTSUPP, "bad version");
    CHECK(setxattr(f, ACC, b, 7, 0) < 0 && errno == EINVAL, "bad size");
    struct ent base[] = { {UO, 7, 0}, {GO, 5, 0}, {O, 1, 0} };
    CHECK(setacl(f, DEF, base, 3) < 0 && errno == EACCES, "default ACL on a file");
    /* an ACL equivalent to a mode is just the mode */
    CHECK(setacl(f, ACC, base, 3) == 0 && fmode(f) == 0751, "equivalent ACL -> mode %o", fmode(f));
    errno = 0;
    CHECK(getxattr(f, ACC, NULL, 0) < 0 && errno == ENODATA, "equivalent ACL not stored");
    CHECK(setacl(f, ACC, a, 6) == 0 && removexattr(f, ACC) == 0, "remove ACL");
    errno = 0;
    CHECK(getxattr(f, ACC, NULL, 0) < 0 && errno == ENODATA, "removed");
    CHECK(try_as(f, O_RDONLY, 1000, 1000, 0) == EACCES, "removed ACL: other bits again");
    char l[256]; snprintf(l, sizeof l, "%s/acl_l", dir);
    unlink(l);
    CHECK(symlink("acl_f", l) == 0, "symlink");
    uint8_t lb2[64]; size_t lbl = blob(lb2, base, 3);
    CHECK(lsetxattr(l, ACC, lb2, lbl, 0) < 0 && errno == EOPNOTSUPP, "no ACL on a symlink");
    CHECK(unlink(l) == 0 && unlink(f) == 0, "unlink");
}

static void test_default(const char *dir) {
    char d[256], f[300], s[300], f2[300], d2[256], f3[300];
    snprintf(d, sizeof d, "%s/acl_d", dir);
    snprintf(f, sizeof f, "%s/file", d); snprintf(s, sizeof s, "%s/sub", d);
    snprintf(f2, sizeof f2, "%s/sub/file", d);
    snprintf(d2, sizeof d2, "%s/acl_d2", dir); snprintf(f3, sizeof f3, "%s/file", d2);
    unlink(f2); rmdir(s); unlink(f); rmdir(d); unlink(f3); rmdir(d2);
    CHECK(mkdir(d, 0755) == 0, "mkdir");
    struct ent def[] = { {UO, 7, 0}, {U, 7, 1000}, {GO, 5, 0}, {M, 7, 0}, {O, 0, 0} };
    CHECK(setacl(d, DEF, def, 5) == 0, "set default ACL");
    CHECK(fmode(d) == 0755, "default ACL leaves mode");
    mode_t old = umask(077);
    int fd = open(f, O_CREAT | O_WRONLY, 0666); close(fd);
    CHECK(fd >= 0 && fmode(f) == 0660, "inherited file mode %o (umask ignored)", fmode(f));
    CHECK(aclperm(f, ACC, U, 1000) == 7 && aclperm(f, ACC, M, 0) == 6, "file access ACL inherited");
    errno = 0;
    CHECK(getxattr(f, DEF, NULL, 0) < 0 && errno == ENODATA, "files get no default ACL");
    CHECK(mkdir(s, 0777) == 0 && fmode(s) == 0770, "subdir mode %o", fmode(s));
    CHECK(aclperm(s, DEF, U, 1000) == 7 && aclperm(s, ACC, M, 0) == 7, "subdir inherits both");
    CHECK(mknod(f2, S_IFREG | 0640, 0) == 0 && fmode(f2) == 0640 && aclperm(f2, ACC, M, 0) == 4, "mknod inherits: %o", fmode(f2));
    umask(old);
    CHECK(try_as(f, O_WRONLY, 1000, 1000, 0) == 0, "named user writes inherited file");
    CHECK(try_as(f, O_RDONLY, 1002, 1002, 0) == EACCES, "other denied inherited file");
    /* a base-only default ACL yields only mode bits */
    CHECK(mkdir(d2, 0755) == 0, "mkdir d2");
    struct ent bdef[] = { {UO, 6, 0}, {GO, 4, 0}, {O, 4, 0} };
    CHECK(setacl(d2, DEF, bdef, 3) == 0, "base default ACL");
    old = umask(077);
    fd = open(f3, O_CREAT | O_WRONLY, 0666); close(fd);
    umask(old);
    CHECK(fmode(f3) == 0644, "base default -> mode %o", fmode(f3));
    errno = 0;
    CHECK(getxattr(f3, ACC, NULL, 0) < 0 && errno == ENODATA, "no access ACL stored");
    CHECK(removexattr(d2, DEF) == 0, "remove default ACL");
    unlink(f3);
    old = umask(077);
    fd = open(f3, O_CREAT | O_WRONLY, 0666); close(fd);
    umask(old);
    CHECK(fmode(f3) == 0600, "umask applies again: %o", fmode(f3));
    CHECK(!unlink(f2) && !rmdir(s) && !unlink(f) && !rmdir(d) && !unlink(f3) && !rmdir(d2), "cleanup");
}

static void persist(const char *dir, int write) {
    char f[256]; snprintf(f, sizeof f, "%s/acl_persist", dir);
    if (write) {
        unlink(f);
        int fd = open(f, O_CREAT | O_WRONLY, 0600); close(fd);
        struct ent a[] = { {UO, 6, 0}, {U, 4, 1000}, {GO, 0, 0}, {M, 4, 0}, {O, 0, 0} };
        CHECK(setacl(f, ACC, a, 5) == 0 && fmode(f) == 0640, "persist set");
        return;
    }
    CHECK(fmode(f) == 0640 && aclperm(f, ACC, U, 1000) == 4, "persisted ACL");
    CHECK(try_as(f, O_RDONLY, 1000, 1000, 0) == 0, "persisted grant");
    CHECK(try_as(f, O_RDONLY, 1001, 0, 0) == EACCES, "persisted group deny");
    CHECK(unlink(f) == 0, "unlink persist");
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
    else { test_access(dir); test_default(dir); }
    if (fails) { printf("acltest: %d failures\n", fails); return 1; }
    printf("acltest: all passed\n");
    return 0;
}
