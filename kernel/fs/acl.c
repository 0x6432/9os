/*
 * POSIX access control lists (M33). ACLs live in the system.posix_acl_access / _default
 * extended attributes (Linux xattr v2 format: u32 version, then {u16 tag, u16 perm, u32 id}
 * entries) of filesystems that store xattrs (ext2: converted to the on-disk v1 format,
 * tmpfs: in memory). Parsed ACLs are cached per inode (i_acl, refcounted; a global
 * sequence number keeps a reader from caching what a concurrent setter replaced).
 *
 * Semantics follow Linux: an access ACL equivalent to the mode bits is not stored; the group
 * mode bits mirror ACL_MASK (or ACL_GROUP_OBJ); chmod rewrites the base entries; a
 * directory's default ACL is inherited by new children instead of applying the umask; the
 * permission algorithm is the POSIX.1e one (owner, named users, groups, other, with mask).
 */
#include <kernel/vfs.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/spinlock.h>
#include <kernel/cred.h>
#include <kernel/process.h>
#include <kernel/sched.h>

enum { A_USER_OBJ = 1, A_USER = 2, A_GROUP_OBJ = 4, A_GROUP = 8, A_MASK = 0x10, A_OTHER = 0x20 };
#define ACL_VERSION 2
#define ACL_NONE ((struct posix_acl *)1)
struct acl_ent { uint16_t tag, perm; uint32_t id; };
struct posix_acl { int refs, n; struct acl_ent e[]; };

static const char *const acl_name[2] = { "system.posix_acl_access", "system.posix_acl_default" };
static spinlock_t acl_lock = SPINLOCK_INIT;
static uint64_t acl_seq;

int acl_type(const char *name) {
    for (int t = 0; t < 2; t++) if (!strcmp(name, acl_name[t])) return t;
    return -1;
}

static struct posix_acl *acl_new(int n) {
    struct posix_acl *a = kzalloc(sizeof *a + (size_t)n * sizeof(struct acl_ent));
    if (a) { a->refs = 1; a->n = n; }
    return a;
}
static void acl_put(struct posix_acl *a) {
    if (a && a != ACL_NONE && !__atomic_sub_fetch(&a->refs, 1, __ATOMIC_ACQ_REL)) kfree(a);
}
static struct posix_acl *acl_dup(const struct posix_acl *a) {
    struct posix_acl *b = acl_new(a->n);
    if (b) memcpy(b->e, a->e, (size_t)a->n * sizeof(struct acl_ent));
    return b;
}

/* xattr v2 blob -> ACL (nullptr + *err=0 for an empty one) */
static struct posix_acl *acl_parse(const void *v, size_t size, int *err) {
    *err = 0;
    if (!v || !size) return nullptr;
    const uint8_t *p = v;
    if (size < 4 || (size - 4) % 8) { *err = -EINVAL; return nullptr; }
    uint32_t ver; memcpy(&ver, p, 4);
    if (ver != ACL_VERSION) { *err = -EOPNOTSUPP; return nullptr; }
    int n = (int)((size - 4) / 8);
    if (!n) return nullptr;
    struct posix_acl *a = acl_new(n);
    if (!a) { *err = -ENOMEM; return nullptr; }
    for (int k = 0; k < n; k++) {
        memcpy(&a->e[k].tag, p + 4 + 8 * k, 2);
        memcpy(&a->e[k].perm, p + 6 + 8 * k, 2);
        memcpy(&a->e[k].id, p + 8 + 8 * k, 4);
        if (a->e[k].tag != A_USER && a->e[k].tag != A_GROUP) a->e[k].id = (uint32_t)-1;
    }
    return a;
}

static int acl_valid(const struct posix_acl *a) {
    int state = A_USER_OBJ, named = 0, mask = 0;
    uint32_t last = 0; bool have = false;
    for (int k = 0; k < a->n; k++) {
        const struct acl_ent *e = &a->e[k];
        if (e->perm & ~7) return -EINVAL;
        switch (e->tag) {
        case A_USER_OBJ: if (state != A_USER_OBJ) return -EINVAL; state = A_USER; break;
        case A_USER:
            if (state != A_USER || (have && e->id <= last)) return -EINVAL;
            last = e->id; have = true; named++; break;
        case A_GROUP_OBJ: if (state != A_USER) return -EINVAL; state = A_GROUP; have = false; break;
        case A_GROUP:
            if (state != A_GROUP || (have && e->id <= last)) return -EINVAL;
            last = e->id; have = true; named++; break;
        case A_MASK: if (state != A_GROUP) return -EINVAL; state = A_OTHER; mask = 1; break;
        case A_OTHER:
            if (state == A_OTHER || (state == A_GROUP && !named)) { state = 0; break; }
            return -EINVAL;
        default: return -EINVAL;
        }
    }
    if (state) return -EINVAL;
    return named && !mask ? -EINVAL : 0;
}

/* the ACL as mode bits: 0 if equivalent (only base entries), 1 if it is needed */
static int acl_equiv_mode(const struct posix_acl *a, uint32_t *mode) {
    uint32_t m = 0; int ne = 0;
    for (int k = 0; k < a->n; k++) {
        const struct acl_ent *e = &a->e[k];
        switch (e->tag) {
        case A_USER_OBJ: m |= (uint32_t)e->perm << 6; break;
        case A_GROUP_OBJ: m |= (uint32_t)e->perm << 3; break;
        case A_OTHER: m |= e->perm; break;
        case A_MASK: m = (m & ~070u) | (uint32_t)e->perm << 3; ne = 1; break;
        default: ne = 1;
        }
    }
    /* GROUP_OBJ may follow MASK in no valid ACL, so the mask wins above */
    *mode = (*mode & ~0777u) | m;
    return ne;
}

/* ---- cache ---- */
static struct posix_acl *acl_get(struct inode *i, int t) {
    if (!i->iops || !i->iops->getxattr) return ACL_NONE;
    uint64_t f = spin_lock_irqsave(&acl_lock);
    struct posix_acl *a = i->i_acl[t];
    if (a && a != ACL_NONE) __atomic_add_fetch(&a->refs, 1, __ATOMIC_RELAXED);
    uint64_t seq = acl_seq;
    spin_unlock_irqrestore(&acl_lock, f);
    if (a) return a;
    int n = i->iops->getxattr(i, acl_name[t], nullptr, 0), err = 0;
    a = ACL_NONE;
    if (n > 0) {
        void *buf = kmalloc((size_t)n);
        if (!buf) return ACL_NONE;
        int m = i->iops->getxattr(i, acl_name[t], buf, (size_t)n);
        struct posix_acl *p = m > 0 ? acl_parse(buf, (size_t)m, &err) : nullptr;
        kfree(buf);
        if (p && acl_valid(p)) { acl_put(p); p = nullptr; }
        if (p) a = p;
        else if (m > 0) return ACL_NONE;        /* unreadable or racing: don't cache */
    }
    f = spin_lock_irqsave(&acl_lock);
    if (!i->i_acl[t] && seq == acl_seq) {
        i->i_acl[t] = a;
        if (a != ACL_NONE) __atomic_add_fetch(&a->refs, 1, __ATOMIC_RELAXED);
    }
    spin_unlock_irqrestore(&acl_lock, f);
    return a;
}

static void acl_invalidate(struct inode *i) {
    uint64_t f = spin_lock_irqsave(&acl_lock);
    struct posix_acl *o[2] = { i->i_acl[0], i->i_acl[1] };
    i->i_acl[0] = i->i_acl[1] = nullptr;
    acl_seq++;
    spin_unlock_irqrestore(&acl_lock, f);
    acl_put(o[0]); acl_put(o[1]);
}
void acl_forget(struct inode *i) {
    acl_put(i->i_acl[0]); acl_put(i->i_acl[1]);
    i->i_acl[0] = i->i_acl[1] = nullptr;
}

static int acl_store(struct inode *i, int t, const struct posix_acl *a) {
    int r;
    if (!a) {
        r = i->iops->setxattr(i, acl_name[t], nullptr, 0, XATTR_REPLACE);
        if (r == -ENODATA) r = 0;
    } else {
        size_t len = 4 + 8 * (size_t)a->n;
        uint8_t *b = kmalloc(len);
        if (!b) return -ENOMEM;
        uint32_t ver = ACL_VERSION; memcpy(b, &ver, 4);
        for (int k = 0; k < a->n; k++) {
            memcpy(b + 4 + 8 * k, &a->e[k].tag, 2);
            memcpy(b + 6 + 8 * k, &a->e[k].perm, 2);
            memcpy(b + 8 + 8 * k, &a->e[k].id, 4);
        }
        r = i->iops->setxattr(i, acl_name[t], b, len, 0);
        kfree(b);
    }
    acl_invalidate(i);
    return r;
}

/* ---- users ---- */
/* 1: no ACL (use the mode bits), 0: granted, -EACCES: denied. Caller: fsuid != owner. */
int acl_permission(const struct cred *c, struct inode *i, int mask) {
    struct posix_acl *a = acl_get(i, 0);
    if (a == ACL_NONE) return 1;
    const struct acl_ent *hit = nullptr, *maskent = nullptr;
    bool found = false, masked = false;
    for (int k = 0; k < a->n; k++) if (a->e[k].tag == A_MASK) maskent = &a->e[k];
    for (int k = 0; k < a->n && !hit; k++) {
        const struct acl_ent *e = &a->e[k];
        switch (e->tag) {
        case A_USER: if (e->id == c->fsuid) { hit = e; masked = true; } break;
        case A_GROUP_OBJ: case A_GROUP:
            if (in_group(c, e->tag == A_GROUP ? e->id : i->gid)) {
                found = true;
                if ((e->perm & mask) == (unsigned)mask) { hit = e; masked = true; }
            }
            break;
        case A_OTHER: if (!found) hit = e; break;
        }
    }
    int r = -EACCES;
    if (hit) {
        unsigned p = hit->perm;
        if (masked && maskent) p &= maskent->perm;
        if ((p & (unsigned)mask) == (unsigned)mask) r = 0;
    }
    acl_put(a);
    return r;
}

/* setxattr/removexattr of an ACL name (val nullptr: remove); permission already checked */
int acl_xattr_set(struct inode *i, int t, const void *val, size_t size) {
    if (!i->iops || !i->iops->setxattr) return -EOPNOTSUPP;
    int r;
    struct posix_acl *a = acl_parse(val, size, &r);
    if (r) return r;
    if (a && (r = acl_valid(a))) { acl_put(a); return r; }
    if (t == 1 && !S_ISDIR(i->mode)) { acl_put(a); return a ? -EACCES : 0; }
    if (t == 0 && a) {
        uint32_t mode = i->mode;
        if (!acl_equiv_mode(a, &mode)) { acl_put(a); a = nullptr; }
        i->mode = mode;
    }
    r = acl_store(i, t, a);
    acl_put(a);
    return r;
}

/* chmod: the base entries follow the new mode */
void acl_chmod(struct inode *i) {
    struct posix_acl *a = acl_get(i, 0);
    if (a == ACL_NONE) return;
    struct posix_acl *b = acl_dup(a);
    acl_put(a);
    if (!b) return;
    struct acl_ent *g = nullptr, *mk = nullptr;
    for (int k = 0; k < b->n; k++)
        switch (b->e[k].tag) {
        case A_USER_OBJ: b->e[k].perm = (i->mode >> 6) & 7; break;
        case A_GROUP_OBJ: g = &b->e[k]; break;
        case A_MASK: mk = &b->e[k]; break;
        case A_OTHER: b->e[k].perm = i->mode & 7; break;
        }
    if (mk) mk->perm = (i->mode >> 3) & 7;
    else if (g) g->perm = (i->mode >> 3) & 7;
    acl_store(i, 0, b);
    acl_put(b);
}

/* mode for a new child of dir: the umask applies only without a default ACL */
uint32_t acl_create_mode(struct inode *dir, uint32_t mode) {
    struct posix_acl *d = acl_get(dir, 1);
    if (d != ACL_NONE) { acl_put(d); return mode; }
    return mode & ~(curproc ? curproc->umask : 022);
}

/* a new inode inherits dir's default ACL (POSIX.1e create masq) */
void acl_inherit(struct inode *dir, struct inode *child) {
    if (S_ISLNK(child->mode) || !child->iops || !child->iops->setxattr) return;
    struct posix_acl *d = acl_get(dir, 1);
    if (d == ACL_NONE) return;
    if (S_ISDIR(child->mode)) acl_store(child, 1, d);
    struct posix_acl *a = acl_dup(d);
    acl_put(d);
    if (!a) return;
    uint32_t mode = child->mode;
    struct acl_ent *g = nullptr, *mk = nullptr;
    int ne = 0;
    for (int k = 0; k < a->n; k++) {
        struct acl_ent *e = &a->e[k];
        switch (e->tag) {
        case A_USER_OBJ: e->perm &= (mode >> 6) & 7; mode &= ((uint32_t)e->perm << 6) | ~0700u; break;
        case A_USER: case A_GROUP: ne = 1; break;
        case A_GROUP_OBJ: g = e; break;
        case A_OTHER: e->perm &= mode & 7; mode &= e->perm | ~07u; break;
        case A_MASK: mk = e; ne = 1; break;
        }
    }
    struct acl_ent *ge = mk ? mk : g;
    if (ge) { ge->perm &= (mode >> 3) & 7; mode &= ((uint32_t)ge->perm << 3) | ~070u; }
    child->mode = mode;
    mark_inode_dirty(child);
    if (ne) acl_store(child, 0, a);
    acl_put(a);
}
