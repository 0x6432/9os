/* Virtual file system: path resolution, open files, generic operations. */
#include <kernel/vfs.h>
#include <kernel/process.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/time.h>
#include <kernel/printk.h>

struct inode *vfs_root;
struct wait_queue poll_wq = WAIT_QUEUE_INIT(poll_wq);
static uint64_t next_ino = 1;

void poll_notify(void) { wake_up(&poll_wq); }

struct timespec now_timespec(void) {
    uint64_t ns = time_ns();
    return (struct timespec){ boot_epoch + (int64_t)(ns / 1000000000ULL), (int64_t)(ns % 1000000000ULL) };
}

struct inode *inode_alloc(uint32_t mode) {
    struct inode *i = kzalloc(sizeof *i);
    if (!i) return nullptr;
    i->mode = mode;
    i->ino = next_ino++;
    i->refcount = 1;
    i->atime = i->mtime = i->ctime = now_timespec();
    return i;
}

void iget(struct inode *i) { if (i) i->refcount++; }
void iput(struct inode *i) {
    if (!i) return;
    if (--i->refcount <= 0 && i->nlink == 0) {
        if (i->iops && i->iops->evict) i->iops->evict(i);
        kfree(i);
    }
}

static struct inode *proc_root(void) {
    return (current && current->proc && current->proc->root) ? current->proc->root : vfs_root;
}
static struct inode *proc_cwd(void) {
    return (current && current->proc && current->proc->cwd) ? current->proc->cwd : vfs_root;
}

static int walk(struct inode *base, const char *path, bool follow_last, int depth, struct inode **out);

static int lookup_child(struct inode *dir, const char *name, struct inode **out) {
    if (!S_ISDIR(dir->mode)) return -ENOTDIR;
    if (!strcmp(name, ".")) { iget(dir); *out = dir; return 0; }
    if (!strcmp(name, "..")) {
        struct inode *d = dir;
        if (d == proc_root()) { iget(d); *out = d; return 0; }
        while (d->covered) d = d->covered;     /* climb out of mounted fs root */
        struct inode *p = d->parent ? d->parent : d;
        iget(p); *out = p;
        return 0;
    }
    if (!dir->iops || !dir->iops->lookup) return -ENOENT;
    struct inode *c;
    int r = dir->iops->lookup(dir, name, &c);
    if (r) return r;
    while (c->mounted) { struct inode *m = c->mounted; iget(m); iput(c); c = m; }
    *out = c;
    return 0;
}

static int walk(struct inode *base, const char *path, bool follow_last, int depth, struct inode **out) {
    if (depth > 40) return -ELOOP;
    if (!*path) return -ENOENT;
    struct inode *cur = path[0] == '/' ? proc_root() : (base ? base : proc_cwd());
    iget(cur);
    const char *p = path;
    char name[256];
    for (;;) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t len = e - p;
        if (len > 255) { iput(cur); return -ENAMETOOLONG; }
        memcpy(name, p, len); name[len] = 0;
        bool last = true;
        for (const char *q = e; *q; q++) if (*q != '/') { last = false; break; }
        struct inode *child;
        int r = lookup_child(cur, name, &child);
        if (r) { iput(cur); return r; }
        if (S_ISLNK(child->mode) && (!last || follow_last || *e == '/') && child->iops->follow_link) {
            struct inode *res;
            r = child->iops->follow_link(child, &res);
            iput(child);
            if (r) { iput(cur); return r; }
            child = res;
        } else if (S_ISLNK(child->mode) && (!last || follow_last || *e == '/')) {
            char *target = kmalloc(4096);
            r = child->iops->readlink(child, target, 4095);
            iput(child);
            if (r < 0) { kfree(target); iput(cur); return r; }
            target[r] = 0;
            struct inode *res;
            r = walk(cur, target, true, depth + 1, &res);
            kfree(target);
            if (r) { iput(cur); return r; }
            child = res;
        }
        iput(cur);
        cur = child;
        if (*e == '/' && last && !S_ISDIR(cur->mode)) { iput(cur); return -ENOTDIR; }
        p = e;
    }
    *out = cur;
    return 0;
}

int vfs_lookup_at(struct inode *base, const char *path, bool follow, struct inode **out) {
    return walk(base, path, follow, 0, out);
}
int vfs_lookup(const char *path, bool follow, struct inode **out) { return walk(nullptr, path, follow, 0, out); }

/* Resolve all but the last component. last receives the final name ("." for "/"). */
int vfs_lookup_parent_at(struct inode *base, const char *path, struct inode **dir, char *last) {
    size_t len = strlen(path);
    if (!len) return -ENOENT;
    while (len > 1 && path[len - 1] == '/') len--;
    size_t s = len;
    while (s > 0 && path[s - 1] != '/') s--;
    size_t nl = len - s;
    if (nl > 255) return -ENAMETOOLONG;
    if (nl == 0) { strcpy(last, "."); } else { memcpy(last, path + s, nl); last[nl] = 0; }
    if (s == 0) {
        struct inode *d = base ? base : proc_cwd();
        iget(d); *dir = d;
        return 0;
    }
    char *dp = kmalloc(s + 1);
    memcpy(dp, path, s); dp[s] = 0;
    int r = walk(base, dp, true, 0, dir);
    kfree(dp);
    if (!r && !S_ISDIR((*dir)->mode)) { iput(*dir); return -ENOTDIR; }
    return r;
}

struct file *file_open_inode(struct inode *ino, int flags) {
    struct file *f = kzalloc(sizeof *f);
    if (!f) return nullptr;
    f->inode = ino;
    iget(ino);
    f->flags = flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC);
    f->refcount = 1;
    f->fops = S_ISCHR(ino->mode) ? chrdev_get(ino->rdev) : ino->fops;
    return f;
}

int vfs_open_at(struct inode *base, const char *path, int flags, uint32_t mode, struct file **out) {
    struct inode *ino;
    int r = walk(base, path, !(flags & O_NOFOLLOW), 0, &ino);
    if (r == -ENOENT && (flags & O_CREAT)) {
        struct inode *dir; char last[256];
        r = vfs_lookup_parent_at(base, path, &dir, last);
        if (r) return r;
        uint32_t um = curproc ? curproc->umask : 022;
        if (!dir->iops->create) { iput(dir); return -EROFS; }
        r = dir->iops->create(dir, last, S_IFREG | (mode & 07777 & ~um), 0, &ino);
        if (!r) fsnotify_dirent(dir, last, IN_CREATE, false, 0);
        iput(dir);
        if (r) return r;
    } else if (r) {
        return r;
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        iput(ino);
        return -EEXIST;
    }
    if (S_ISLNK(ino->mode) && (flags & O_NOFOLLOW) && !(flags & O_PATH)) { iput(ino); return -ELOOP; }
    if ((flags & O_DIRECTORY) && !S_ISDIR(ino->mode)) { iput(ino); return -ENOTDIR; }
    if (S_ISDIR(ino->mode) && (flags & O_ACCMODE) != O_RDONLY) { iput(ino); return -EISDIR; }
    if ((flags & O_TRUNC) && S_ISREG(ino->mode) && (flags & O_ACCMODE) != O_RDONLY && ino->iops->truncate)
        ino->iops->truncate(ino, 0);
    struct file *f = file_open_inode(ino, flags);
    iput(ino);
    if (!f) return -ENOMEM;
    if (path[0] == '/') f->path = strdup(path);
    else {
        char *cwd = kmalloc(4096);
        int l = vfs_getcwd(base ? base : proc_cwd(), cwd, 4096);
        if (l > 0) {
            size_t pl = strlen(path);
            char *full = kmalloc(l + pl + 2);
            snprintf(full, l + pl + 2, "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", path);
            f->path = full;
        }
        kfree(cwd);
    }
    if (S_ISCHR(ino->mode) && !f->fops) { vfs_close(f); return -ENXIO; }
    if (f->fops && f->fops->open && !(flags & O_PATH)) {
        r = f->fops->open(ino, f);
        if (r) { f->fops = nullptr; vfs_close(f); return r; }
    }
    if (!(flags & O_PATH)) fsnotify_file(f, IN_OPEN);
    *out = f;
    return 0;
}
int vfs_open(const char *path, int flags, uint32_t mode, struct file **out) {
    return vfs_open_at(nullptr, path, flags, mode, out);
}

void vfs_close(struct file *f) {
    if (--f->refcount > 0) return;
    if (f->inode && (S_ISREG(f->inode->mode) || S_ISDIR(f->inode->mode)) && !(f->flags & O_PATH))
        fsnotify_file(f, (f->flags & O_ACCMODE) != O_RDONLY ? IN_CLOSE_WRITE_ : IN_CLOSE_NOWRITE);
    if (f->fops && f->fops->release) f->fops->release(f);
    iput(f->inode);
    kfree(f->path);
    kfree(f);
}

ssize_t vfs_read(struct file *f, void *buf, size_t n) {
    if ((f->flags & O_ACCMODE) == O_WRONLY || (f->flags & O_PATH)) return -EBADF;
    if (S_ISDIR(f->inode->mode)) return -EISDIR;
    if (!f->fops || !f->fops->read) return -EINVAL;
    ssize_t r = f->fops->read(f, buf, n, &f->pos);
    if (r > 0 && S_ISREG(f->inode->mode)) fsnotify_file(f, IN_ACCESS);
    return r;
}

ssize_t vfs_write(struct file *f, const void *buf, size_t n) {
    if ((f->flags & O_ACCMODE) == O_RDONLY || (f->flags & O_PATH)) return -EBADF;
    if (!f->fops || !f->fops->write) return -EINVAL;
    if ((f->flags & O_APPEND) && S_ISREG(f->inode->mode)) f->pos = f->inode->size;
    ssize_t r = f->fops->write(f, buf, n, &f->pos);
    if (r > 0 && S_ISREG(f->inode->mode)) fsnotify_file(f, IN_MODIFY_);
    return r;
}

ssize_t vfs_pread(struct file *f, void *buf, size_t n, off_t off) {
    if (!f->fops || !f->fops->read) return -EINVAL;
    return f->fops->read(f, buf, n, &off);
}

int vfs_mknod_at(struct inode *base, const char *path, uint32_t mode, uint64_t rdev) {
    struct inode *dir, *ino; char last[256];
    int r = vfs_lookup_parent_at(base, path, &dir, last);
    if (r) return r;
    if (!dir->iops->create) { iput(dir); return -EROFS; }
    struct inode *ex;
    if (!lookup_child(dir, last, &ex)) { iput(ex); iput(dir); return -EEXIST; }
    r = dir->iops->create(dir, last, mode, rdev, &ino);
    if (!r) { fsnotify_dirent(dir, last, IN_CREATE, S_ISDIR(mode), 0); iput(ino); }
    iput(dir);
    return r;
}

int vfs_mkdir_at(struct inode *base, const char *path, uint32_t mode) {
    uint32_t um = curproc ? curproc->umask : 022;
    return vfs_mknod_at(base, path, S_IFDIR | (mode & 07777 & ~um), 0);
}

int vfs_unlink_at(struct inode *base, const char *path, bool rmdir) {
    struct inode *dir; char last[256];
    int r = vfs_lookup_parent_at(base, path, &dir, last);
    if (r) return r;
    if (!strcmp(last, ".") || !strcmp(last, "..")) { iput(dir); return rmdir ? -EINVAL : -EISDIR; }
    struct inode *victim = nullptr;
    if (fsnotify_nwatches && lookup_child(dir, last, &victim)) victim = nullptr;
    r = dir->iops->unlink ? dir->iops->unlink(dir, last, rmdir) : -EROFS;
    if (!r && victim) {
        fsnotify_dirent(dir, last, IN_DELETE, S_ISDIR(victim->mode), 0);
        fsnotify_unlinked(victim);
    }
    if (victim) iput(victim);
    iput(dir);
    return r;
}

int vfs_symlink_at(struct inode *base, const char *target, const char *path) {
    struct inode *dir; char last[256];
    int r = vfs_lookup_parent_at(base, path, &dir, last);
    if (r) return r;
    struct inode *ex;
    if (!lookup_child(dir, last, &ex)) { iput(ex); iput(dir); return -EEXIST; }
    r = dir->iops->symlink ? dir->iops->symlink(dir, last, target) : -EROFS;
    if (!r) fsnotify_dirent(dir, last, IN_CREATE, false, 0);
    iput(dir);
    return r;
}

int vfs_link_at(struct inode *ob, const char *opath, struct inode *nb, const char *npath, bool follow) {
    struct inode *src;
    int r = walk(ob, opath, follow, 0, &src);
    if (r) return r;
    if (S_ISDIR(src->mode)) { iput(src); return -EPERM; }
    struct inode *dir; char last[256];
    r = vfs_lookup_parent_at(nb, npath, &dir, last);
    if (r) { iput(src); return r; }
    struct inode *ex;
    if (!lookup_child(dir, last, &ex)) { iput(ex); r = -EEXIST; }
    else r = dir->iops->link ? dir->iops->link(dir, last, src) : -EPERM;
    if (!r) { fsnotify_dirent(dir, last, IN_CREATE, false, 0); fsnotify_inode(src, IN_ATTRIB); }
    iput(dir); iput(src);
    return r;
}

int vfs_rename_at(struct inode *ob, const char *opath, struct inode *nb, const char *npath) {
    struct inode *od, *nd; char ol[256], nl[256];
    int r = vfs_lookup_parent_at(ob, opath, &od, ol);
    if (r) return r;
    r = vfs_lookup_parent_at(nb, npath, &nd, nl);
    if (r) { iput(od); return r; }
    struct inode *moved = nullptr;
    if (fsnotify_nwatches && lookup_child(od, ol, &moved)) moved = nullptr;
    if (od->iops != nd->iops) r = -EXDEV;
    else r = od->iops->rename ? od->iops->rename(od, ol, nd, nl) : -EROFS;
    if (!r && moved) {
        uint32_t ck = fsnotify_cookie();
        bool d = S_ISDIR(moved->mode);
        fsnotify_dirent(od, ol, IN_MOVED_FROM, d, ck);
        fsnotify_dirent(nd, nl, IN_MOVED_TO, d, ck);
        fsnotify_inode(moved, IN_MOVE_SELF);
    }
    if (moved) iput(moved);
    iput(od); iput(nd);
    return r;
}

int vfs_readlink_at(struct inode *base, const char *path, char *buf, size_t size) {
    struct inode *i;
    int r = walk(base, path, false, 0, &i);
    if (r) return r;
    r = S_ISLNK(i->mode) && i->iops->readlink ? i->iops->readlink(i, buf, size) : -EINVAL;
    iput(i);
    return r;
}

void vfs_stat(struct inode *i, struct kstat *st) {
    memset(st, 0, sizeof *st);
    st->dev = i->dev ? i->dev : 1;
    st->ino = i->ino;
    st->nlink = i->nlink ? i->nlink : 1;
    st->mode = i->mode;
    st->uid = i->uid; st->gid = i->gid;
    st->rdev = i->rdev;
    st->size = i->size;
    st->blksize = 4096;
    st->blocks = (i->size + 511) / 512;
    st->atime = i->atime; st->mtime = i->mtime; st->ctime = i->ctime;
}

int vfs_mount(const char *path, struct inode *root) {
    struct inode *mp;
    int r = vfs_lookup(path, true, &mp);
    if (r) return r;
    if (!S_ISDIR(mp->mode)) { iput(mp); return -ENOTDIR; }
    mp->mounted = root;
    root->covered = mp;
    root->parent = mp->parent;
    return 0;
}

struct find_ctx { uint64_t ino; char *name; bool found; };
static int find_fill(void *c, const char *name, size_t len, uint64_t ino, unsigned type) {
    struct find_ctx *fc = c;
    if (ino == fc->ino && strcmp(name, ".") && strcmp(name, "..")) {
        memcpy(fc->name, name, MIN(len, 255)); fc->name[MIN(len, 255)] = 0;
        fc->found = true;
        return 1;
    }
    return 0;
}

int vfs_getcwd(struct inode *cwd, char *buf, size_t size) {
    char *tmp = kmalloc(4096), name[256];
    size_t pos = 4095;
    tmp[pos] = 0;
    struct inode *cur = cwd, *root = proc_root();
    while (cur != root) {
        struct inode *c = cur;
        while (c->covered) c = c->covered;
        struct inode *parent = c->parent;
        if (!parent || parent == c) break;
        struct find_ctx fc = { c->ino, name, false };
        uint64_t p = 0;
        if (parent->iops && parent->iops->iterate) parent->iops->iterate(parent, &p, find_fill, &fc);
        if (!fc.found) { kfree(tmp); return -ENOENT; }
        size_t l = strlen(name);
        if (l + 1 > pos) { kfree(tmp); return -ENAMETOOLONG; }
        pos -= l; memcpy(tmp + pos, name, l);
        tmp[--pos] = '/';
        cur = parent;
    }
    if (pos == 4095) tmp[--pos] = '/';
    size_t len = 4095 - pos + 1;
    if (len > size) { kfree(tmp); return -ERANGE; }
    memcpy(buf, tmp + pos, len);
    kfree(tmp);
    return (int)len;
}

void vfs_init(void) {
    vfs_root = tmpfs_create_root();
    vfs_root->parent = vfs_root;
    pr_info("vfs: tmpfs root mounted\n");
}
