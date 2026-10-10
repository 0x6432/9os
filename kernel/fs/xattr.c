/*
 * Extended attributes (M33): the *xattr syscall family, namespace permission rules, and an
 * in-memory implementation for tmpfs (simple_*xattr). Filesystems provide inode_ops
 * getxattr/setxattr/listxattr on full names; ext2 stores them in an EA block (ext2.c).
 *
 * Namespaces: user.* (regular files and directories only, DAC read/write permission),
 * trusted.* (CAP_SYS_ADMIN; hidden from listxattr otherwise), security.* (anyone reads,
 * CAP_SYS_ADMIN writes, no LSM), system.posix_acl_access/default (anyone reads, the owner or
 * CAP_FOWNER writes, validated and mirrored into the mode by acl.c). Others: EOPNOTSUPP.
 */
#include <kernel/vfs.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/list.h>
#include <kernel/mutex.h>
#include <kernel/mm.h>
#include <kernel/cred.h>
#include <kernel/syscall.h>

#define PATH_MAX 4096
int user_path(const char *upath, char *kpath);
int dirfd_base(int dirfd, const char *path, struct inode **base);

/* ---- in-memory attributes ---- */
struct xent { struct list_node node; size_t vlen; uint8_t *val; char name[]; };
static const struct lock_class xmem_class = { "xattr_mem", LR_MUTEX_MISC, false };
static struct mutex xmem_lock = MUTEX_INIT(xmem_lock, &xmem_class);

static struct xent *xfind(struct inode *i, const char *name) {
    if (!i->xattrs) return nullptr;
    list_for_each(it, i->xattrs) {
        struct xent *e = list_entry(it, struct xent, node);
        if (!strcmp(e->name, name)) return e;
    }
    return nullptr;
}

int simple_getxattr(struct inode *i, const char *name, void *buf, size_t size) {
    mutex_lock(&xmem_lock);
    struct xent *e = xfind(i, name);
    int r = !e ? -ENODATA : !size ? (int)e->vlen : size < e->vlen ? -ERANGE : (int)e->vlen;
    if (e && size && r >= 0) memcpy(buf, e->val, e->vlen);
    mutex_unlock(&xmem_lock);
    return r;
}

int simple_setxattr(struct inode *i, const char *name, const void *val, size_t size, int flags) {
    struct xent *n = nullptr;
    uint8_t *v = nullptr;
    struct list_node *head = nullptr;
    if (val) {
        size_t l = strlen(name);
        n = kmalloc(sizeof *n + l + 1);
        v = size ? kmalloc(size) : nullptr;
        head = i->xattrs ? nullptr : kmalloc(sizeof *head);
        if (!n || (size && !v) || (!i->xattrs && !head)) { kfree(n); kfree(v); kfree(head); return -ENOSPC; }
        memcpy(n->name, name, l + 1);
        if (size) memcpy(v, val, size);
        n->val = v; n->vlen = size;
    }
    mutex_lock(&xmem_lock);
    struct xent *e = xfind(i, name);
    int r = 0;
    if ((flags & XATTR_CREATE) && e) r = -EEXIST;
    else if ((flags & XATTR_REPLACE || !val) && !e) r = -ENODATA;
    if (!r) {
        if (e) { list_del(&e->node); kfree(e->val); kfree(e); }
        if (n) {
            if (!i->xattrs) { list_init(head); i->xattrs = head; head = nullptr; }
            list_add_tail(i->xattrs, &n->node);
            n = nullptr; v = nullptr;
        }
    }
    mutex_unlock(&xmem_lock);
    kfree(n); kfree(v); kfree(head);
    return r;
}

int simple_listxattr(struct inode *i, char *buf, size_t size) {
    mutex_lock(&xmem_lock);
    size_t tot = 0;
    int r = 0;
    if (i->xattrs) list_for_each(it, i->xattrs) {
        struct xent *e = list_entry(it, struct xent, node);
        size_t l = strlen(e->name) + 1;
        if (size && tot + l > size) { r = -ERANGE; break; }
        if (size) memcpy(buf + tot, e->name, l);
        tot += l;
    }
    mutex_unlock(&xmem_lock);
    return r ? r : (int)tot;
}

void simple_xattrs_free(struct inode *i) {
    if (!i->xattrs) return;
    list_for_each_safe(it, tmp, i->xattrs) {
        struct xent *e = list_entry(it, struct xent, node);
        kfree(e->val); kfree(e);
    }
    kfree(i->xattrs);
    i->xattrs = nullptr;
}

/* ---- permission rules ---- */
enum { NS_USER, NS_TRUSTED, NS_SECURITY, NS_ACL, NS_BAD };
static int ns_of(const char *name) {
    if (!strncmp(name, "user.", 5) && name[5]) return NS_USER;
    if (!strncmp(name, "trusted.", 8) && name[8]) return NS_TRUSTED;
    if (!strncmp(name, "security.", 9) && name[9]) return NS_SECURITY;
    if (acl_type(name) >= 0) return NS_ACL;
    return NS_BAD;
}

static int xattr_perm(struct inode *i, const char *name, int mask) {
    int ns = ns_of(name);
    if (ns == NS_BAD) return -EOPNOTSUPP;
    if ((mask & MAY_WRITE) && i->sb && (i->sb->flags & SB_RDONLY)) return -EROFS;
    switch (ns) {
    case NS_TRUSTED: return capable(CAP_SYS_ADMIN) ? 0 : -EPERM;
    case NS_SECURITY: return (mask & MAY_WRITE) && !capable(CAP_SYS_ADMIN) ? -EPERM : 0;
    case NS_ACL: if (S_ISLNK(i->mode)) return -EOPNOTSUPP;     /* as Linux: no ACLs on symlinks */
        return (mask & MAY_WRITE) && !inode_owner_or_capable(i) ? -EPERM : 0;
    default:
        if (!S_ISREG(i->mode) && !S_ISDIR(i->mode)) return mask & MAY_WRITE ? -EPERM : -ENODATA;
        return inode_permission(i, mask);
    }
}

/* ---- syscall bodies on an inode ---- */
static int64_t get_name(const char *uname, char *kname) {
    if (!uname) return -EFAULT;
    int64_t r = strncpy_from_user(kname, uname, XATTR_NAME_MAX + 1);
    if (r == -ENAMETOOLONG) return -ERANGE;
    if (r < 0) return r;
    return r ? 0 : -ERANGE;
}

static int64_t do_setxattr(struct inode *i, const char *uname, const void *uval, size_t size, int flags) {
    if (flags & ~(XATTR_CREATE | XATTR_REPLACE)) return -EINVAL;
    if (size > XATTR_SIZE_MAX) return -E2BIG;
    char name[XATTR_NAME_MAX + 1];
    int64_t r = get_name(uname, name);
    if (r) return r;
    if ((r = xattr_perm(i, name, MAY_WRITE))) return r;
    if (!i->iops || !i->iops->setxattr) return -EOPNOTSUPP;
    void *kv = kmalloc(size ? size : 1);
    if (!kv) return -ENOMEM;
    if (size && copy_from_user(kv, uval, size)) { kfree(kv); return -EFAULT; }
    int t = acl_type(name);
    r = t >= 0 ? acl_xattr_set(i, t, kv, size) : i->iops->setxattr(i, name, kv, size, flags);
    kfree(kv);
    if (!r) { i->ctime = now_timespec(); mark_inode_dirty(i); fsnotify_inode(i, IN_ATTRIB); }
    return r;
}

static int64_t do_getxattr(struct inode *i, const char *uname, void *uval, size_t size) {
    char name[XATTR_NAME_MAX + 1];
    int64_t r = get_name(uname, name);
    if (r) return r;
    if ((r = xattr_perm(i, name, MAY_READ))) return r;
    if (!i->iops || !i->iops->getxattr) return -EOPNOTSUPP;
    if (size > XATTR_SIZE_MAX) size = XATTR_SIZE_MAX;
    void *kv = size ? kmalloc(size) : nullptr;
    if (size && !kv) return -ENOMEM;
    r = i->iops->getxattr(i, name, kv, size);
    if (r > 0 && size && copy_to_user(uval, kv, (size_t)r)) r = -EFAULT;
    kfree(kv);
    return r;
}

static int64_t do_removexattr(struct inode *i, const char *uname) {
    char name[XATTR_NAME_MAX + 1];
    int64_t r = get_name(uname, name);
    if (r) return r;
    if ((r = xattr_perm(i, name, MAY_WRITE))) return r;
    if (!i->iops || !i->iops->setxattr) return -EOPNOTSUPP;
    int t = acl_type(name);
    r = t >= 0 ? acl_xattr_set(i, t, nullptr, 0) : i->iops->setxattr(i, name, nullptr, 0, XATTR_REPLACE);
    if (!r) { i->ctime = now_timespec(); mark_inode_dirty(i); fsnotify_inode(i, IN_ATTRIB); }
    return r;
}

/* names the caller may see: trusted.* only with CAP_SYS_ADMIN, user.* only on files/dirs */
static int64_t do_listxattr(struct inode *i, char *ubuf, size_t size) {
    if (!i->iops || !i->iops->listxattr) return 0;
    int n = i->iops->listxattr(i, nullptr, 0);
    if (n <= 0) return n;
    char *k = kmalloc((size_t)n + 1);
    if (!k) return -ENOMEM;
    int m = i->iops->listxattr(i, k, (size_t)n);     /* may race with a setxattr: retry once */
    if (m == -ERANGE) { kfree(k); return -ERANGE; }
    if (m < 0) { kfree(k); return m; }
    bool admin = capable(CAP_SYS_ADMIN), ud = S_ISREG(i->mode) || S_ISDIR(i->mode);
    size_t out = 0;
    for (int p = 0; p < m;) {
        size_t l = strlen(k + p) + 1;
        int ns = ns_of(k + p);
        if ((ns != NS_TRUSTED || admin) && (ns != NS_USER || ud)) { memmove(k + out, k + p, l); out += l; }
        p += (int)l;
    }
    int64_t r = (int64_t)out;
    if (size) {
        if (out > size) r = -ERANGE;
        else if (out && copy_to_user(ubuf, k, out)) r = -EFAULT;
    }
    kfree(k);
    return r;
}

/* ---- syscalls ---- */
static int64_t path_inode(const char *upath, bool follow, struct inode **out) {
    char *path = kmalloc(PATH_MAX);
    if (!path) return -ENOMEM;
    int64_t r = user_path(upath, path);
    struct inode *base = nullptr;
    if (!r) r = dirfd_base(AT_FDCWD, path, &base);
    if (!r) r = vfs_lookup_at(base, path, follow, out);
    kfree(path);
    return r;
}

#define PATH_CALL(upath, follow, expr) do { \
    struct inode *i; int64_t __r = path_inode(upath, follow, &i); \
    if (__r) return __r; __r = (expr); iput(i); return __r; } while (0)
#define FD_CALL(fd, expr) do { \
    struct file *f = fd_get(fd); if (!f) return -EBADF; \
    struct inode *i = f->inode; return (expr); } while (0)

int64_t sys_setxattr(const char *p, const char *n, const void *v, size_t s, int fl) { PATH_CALL(p, true, do_setxattr(i, n, v, s, fl)); }
int64_t sys_lsetxattr(const char *p, const char *n, const void *v, size_t s, int fl) { PATH_CALL(p, false, do_setxattr(i, n, v, s, fl)); }
int64_t sys_fsetxattr(int fd, const char *n, const void *v, size_t s, int fl) { FD_CALL(fd, do_setxattr(i, n, v, s, fl)); }
int64_t sys_getxattr(const char *p, const char *n, void *v, size_t s) { PATH_CALL(p, true, do_getxattr(i, n, v, s)); }
int64_t sys_lgetxattr(const char *p, const char *n, void *v, size_t s) { PATH_CALL(p, false, do_getxattr(i, n, v, s)); }
int64_t sys_fgetxattr(int fd, const char *n, void *v, size_t s) { FD_CALL(fd, do_getxattr(i, n, v, s)); }
int64_t sys_listxattr(const char *p, char *b, size_t s) { PATH_CALL(p, true, do_listxattr(i, b, s)); }
int64_t sys_llistxattr(const char *p, char *b, size_t s) { PATH_CALL(p, false, do_listxattr(i, b, s)); }
int64_t sys_flistxattr(int fd, char *b, size_t s) { FD_CALL(fd, do_listxattr(i, b, s)); }
int64_t sys_removexattr(const char *p, const char *n) { PATH_CALL(p, true, do_removexattr(i, n)); }
int64_t sys_lremovexattr(const char *p, const char *n) { PATH_CALL(p, false, do_removexattr(i, n)); }
int64_t sys_fremovexattr(int fd, const char *n) { FD_CALL(fd, do_removexattr(i, n)); }
