/* shared by getfacl.c / setfacl.c: POSIX ACLs as raw system.posix_acl_* xattrs (v2 format) */
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

enum { UO = 1, U = 2, GO = 4, G = 8, M = 0x10, O = 0x20 };
#define ACC "system.posix_acl_access"
#define DEF "system.posix_acl_default"
#define MAXE 128
struct ent { uint16_t tag, perm; uint32_t id; };
struct acl { int n; struct ent e[MAXE]; };

/* 1 if the ACL exists, 0 if not, -1 on error */
static int acl_read(const char *path, const char *name, struct acl *a) {
    uint8_t b[4 + 8 * MAXE];
    a->n = 0;
    ssize_t n = getxattr(path, name, b, sizeof b);
    if (n < 0) return errno == ENODATA || errno == EOPNOTSUPP ? 0 : -1;
    if (n < 4 || (n - 4) % 8) { errno = EINVAL; return -1; }
    a->n = (int)((n - 4) / 8);
    for (int k = 0; k < a->n; k++) memcpy(&a->e[k], b + 4 + 8 * k, 8);
    return a->n > 0;
}
static void acl_from_mode(struct acl *a, mode_t m) {
    a->n = 3;
    a->e[0] = (struct ent){ UO, (m >> 6) & 7, (uint32_t)-1 };
    a->e[1] = (struct ent){ GO, (m >> 3) & 7, (uint32_t)-1 };
    a->e[2] = (struct ent){ O, m & 7, (uint32_t)-1 };
}
static int ent_cmp(const void *x, const void *y) {
    const struct ent *a = x, *b = y;
    if (a->tag != b->tag) return a->tag < b->tag ? -1 : 1;
    return a->id < b->id ? -1 : a->id > b->id;
}
static int acl_write(const char *path, const char *name, struct acl *a) {
    if (!a->n) return removexattr(path, name) < 0 && errno != ENODATA ? -1 : 0;
    qsort(a->e, a->n, sizeof a->e[0], ent_cmp);
    uint8_t b[4 + 8 * MAXE];
    uint32_t v = 2; memcpy(b, &v, 4);
    for (int k = 0; k < a->n; k++) {
        if (a->e[k].tag != U && a->e[k].tag != G) a->e[k].id = (uint32_t)-1;
        memcpy(b + 4 + 8 * k, &a->e[k], 8);
    }
    return setxattr(path, name, b, 4 + 8 * (size_t)a->n, 0);
}
static struct ent *acl_find(struct acl *a, int tag, uint32_t id) {
    for (int k = 0; k < a->n; k++)
        if (a->e[k].tag == tag && ((tag != U && tag != G) || a->e[k].id == id)) return &a->e[k];
    return NULL;
}
static void perm_str(unsigned p, char *s) { s[0] = p & 4 ? 'r' : '-'; s[1] = p & 2 ? 'w' : '-'; s[2] = p & 1 ? 'x' : '-'; s[3] = 0; }
