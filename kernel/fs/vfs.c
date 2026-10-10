#include <kernel/mutex.h>
/* Virtual file system: path resolution, open files, generic operations. */
#include <kernel/vfs.h>
#include <kernel/process.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/time.h>
#include <kernel/blk.h>
#include <kernel/pagecache.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>

struct inode *vfs_root;
struct wait_queue poll_wq = WAIT_QUEUE_INIT(poll_wq);
static uint64_t next_ino = 1;

/*
 * Namespace mutex (M24): path walks, directory contents, mounts and process cwd/root changes
 * are serialised by this recursive sleeping mutex instead of the BKL (cf. Linux's early
 * dcache_lock/rename_lock). File data has per-inode locks (tmpfs.c); filesystems whose
 * callbacks still need the BKL (procfs) take it themselves.
 */
static const struct lock_class ns_class = { "vfs_ns", LR_MUTEX_VFS, false };
static struct mutex ns_mtx = MUTEX_INIT(ns_mtx, &ns_class);
static int ns_depth;
void vfs_ns_lock(void) {
    if (mutex_owned(&ns_mtx)) { ns_depth++; return; }
    mutex_lock(&ns_mtx);
    ns_depth = 1;
}
void vfs_ns_unlock(void) {
    if (--ns_depth > 0) return;
    mutex_unlock(&ns_mtx);
}

uint64_t poll_seq;
void poll_notify(void) { __atomic_add_fetch(&poll_seq, 1, __ATOMIC_SEQ_CST); wake_up(&poll_wq); }

/* sleep on poll_wq unless poll_notify() ran since 'seq' was sampled (before the readiness scan) */
int poll_wait_seq(uint64_t seq, uint64_t ns) {
    uint64_t f = sched_wait_lock();
    if (__atomic_load_n(&poll_seq, __ATOMIC_SEQ_CST) != seq) { sched_wait_unlock(f); return 0; }
    return wait_event_timeout_locked(&poll_wq, ns, f);
}

struct timespec now_timespec(void) {
    uint64_t ns = time_ns();
    return (struct timespec){ boot_epoch + (int64_t)(ns / 1000000000ULL), (int64_t)(ns % 1000000000ULL) };
}

static const struct lock_class i_mmap_class = { "i_mmap", LR_I_MMAP, false };
static const struct lock_class icache_class = { "icache", LR_ICACHE, false };

void inode_init(struct inode *i, uint32_t mode) {
    i->mode = mode;
    i->uid = current_cred()->fsuid;      /* pipes, sockets, anonymous files: the creator */
    i->gid = current_cred()->fsgid;
    i->refcount = 1;
    i->atime = i->mtime = i->ctime = now_timespec();
    list_init(&i->i_mmap);
    list_init(&i->i_hash);
    list_init(&i->i_lru);
    spin_lock_init_class(&i->i_mmap_lock, &i_mmap_class);
}

struct inode *inode_alloc(uint32_t mode) {
    struct inode *i = kzalloc(sizeof *i);
    if (!i) return nullptr;
    inode_init(i, mode);
    i->ino = __atomic_fetch_add(&next_ino, 1, __ATOMIC_RELAXED);
    return i;
}

/* ---- inode cache of disk filesystems (see vfs.h) ---- */
#define ICACHE_HASH 512
#define ICACHE_MAX_UNUSED 4096
uint64_t icache_hits, icache_misses, icache_evictions;

void sb_init(struct super_block *sb, const struct super_ops *ops, const char *type) {
    sb->ops = ops;
    sb->type = type;
    spin_lock_init_class(&sb->icache_lock, &icache_class);
    sb->ihash = kmalloc(ICACHE_HASH * sizeof *sb->ihash);
    for (int k = 0; k < ICACHE_HASH; k++) list_init(&sb->ihash[k]);
    list_init(&sb->ilru);
}
static uint64_t ic_lock(struct super_block *sb) { uint64_t f = arch_irq_save(); spin_lock_ipi(&sb->icache_lock); return f; }
static void ic_unlock(struct super_block *sb, uint64_t f) { spin_unlock(&sb->icache_lock); arch_irq_restore(f); }

struct inode *icache_find(struct super_block *sb, uint64_t ino) {
    uint64_t f = ic_lock(sb);
    list_for_each(it, &sb->ihash[ino % ICACHE_HASH]) {
        struct inode *i = list_entry(it, struct inode, i_hash);
        if (i->ino != ino) continue;
        if (__atomic_add_fetch(&i->refcount, 1, __ATOMIC_ACQ_REL) == 1 && (i->i_state & I_LRU)) {
            list_del(&i->i_lru); list_init(&i->i_lru);
            i->i_state &= ~I_LRU;
            sb->nunused--;
        }
        ic_unlock(sb, f);
        icache_hits++;
        return i;
    }
    ic_unlock(sb, f);
    icache_misses++;
    return nullptr;
}

void icache_insert(struct super_block *sb, struct inode *i) {
    i->sb = sb;
    uint64_t f = ic_lock(sb);
    list_add(&sb->ihash[i->ino % ICACHE_HASH], &i->i_hash);
    sb->ninodes++;
    ic_unlock(sb, f);
}

/* unhash (lock held); the caller evicts */
static void ic_unhash(struct super_block *sb, struct inode *i) {
    list_del(&i->i_hash); list_init(&i->i_hash);
    if (i->i_state & I_LRU) { list_del(&i->i_lru); list_init(&i->i_lru); i->i_state &= ~I_LRU; sb->nunused--; }
    sb->ninodes--;
}

static bool ic_detach(struct inode *i, bool discard) {
    return i->mapping ? mapping_detach(i->mapping, &i->refcount, discard) : __atomic_load_n(&i->refcount, __ATOMIC_ACQUIRE) == 0;
}

/* evict up to n clean unused inodes, oldest first */
static void icache_prune(struct super_block *sb, unsigned n) {
    uint64_t f = ic_lock(sb);
    if (sb->pruning) { ic_unlock(sb, f); return; }
    sb->pruning = true;
    for (unsigned scanned = 0; n && scanned < 4 * n + 16 && !list_empty(&sb->ilru); scanned++) {
        struct inode *i = list_first(&sb->ilru, struct inode, i_lru);
        list_del(&i->i_lru); list_init(&i->i_lru);
        i->i_state &= ~I_LRU;
        sb->nunused--;
        if (__atomic_load_n(&i->refcount, __ATOMIC_ACQUIRE) || i == sb->root) continue;   /* in use again */
        if (!ic_detach(i, false)) {         /* dirty data: keep it until written back */
            list_add_tail(&sb->ilru, &i->i_lru); i->i_state |= I_LRU; sb->nunused++;
            continue;
        }
        ic_unhash(sb, i);
        ic_unlock(sb, f);
        icache_evictions++;
        sb->ops->evict_inode(i);
        n--;
        f = ic_lock(sb);
    }
    sb->pruning = false;
    ic_unlock(sb, f);
}

static void sb_iput(struct inode *i) {
    struct super_block *sb = i->sb;
    uint64_t f = ic_lock(sb);
    if (__atomic_sub_fetch(&i->refcount, 1, __ATOMIC_ACQ_REL) > 0) { ic_unlock(sb, f); return; }
    bool dying = i->nlink == 0 || (sb->flags & SB_DYING);
    if (dying && i != sb->root) {
        if (!ic_detach(i, i->nlink == 0)) { ic_unlock(sb, f); return; }   /* raced with writeback's iget */
        ic_unhash(sb, i);
        ic_unlock(sb, f);
        sb->ops->evict_inode(i);
        return;
    }
    if (!(i->i_state & I_LRU)) { list_add_tail(&sb->ilru, &i->i_lru); i->i_state |= I_LRU; sb->nunused++; }
    bool prune = sb->nunused > ICACHE_MAX_UNUSED;
    ic_unlock(sb, f);
    if (prune) icache_prune(sb, 64);
}

void iget(struct inode *i) { if (i) __atomic_add_fetch(&i->refcount, 1, __ATOMIC_RELAXED); }
void iput(struct inode *i) {
    if (!i) return;
    if (i->sb) { sb_iput(i); return; }
    if (__atomic_sub_fetch(&i->refcount, 1, __ATOMIC_ACQ_REL) <= 0 && i->nlink == 0) {
        if (i->iops && i->iops->evict) i->iops->evict(i);
        simple_xattrs_free(i);
        acl_forget(i);
        kfree(i);
    }
}

void mark_inode_dirty(struct inode *i) {
    if (i->sb && i->sb->ops->write_inode) i->sb->ops->write_inode(i);
}

static struct inode *proc_root(void) {
    return (current && current->proc && current->proc->root) ? current->proc->root : vfs_root;
}
static struct inode *proc_cwd(void) {
    return (current && current->proc && current->proc->cwd) ? current->proc->cwd : vfs_root;
}


/*
 * Directory-entry cache (M30) for filesystems with SB_DCACHE (ext2): (dir, name) -> inode, or
 * a negative entry for a name known not to exist (PATH searches, failed opens). Entries pin
 * their inode and directory; the cache is bounded by an LRU. Everything runs under the
 * namespace mutex, which every name-changing operation also holds, so the wrappers below
 * invalidate the affected names before/after calling the filesystem.
 */
#define DC_HASH 1024
#define DC_MAX 8192
struct dentry { struct list_node hnode, lru; struct inode *dir, *ino; uint32_t hash; char name[]; };
static struct list_node dc_hash[DC_HASH];
static struct list_node dc_lru = LIST_INIT(dc_lru);
static unsigned dc_count;
uint64_t dcache_hits, dcache_neg_hits, dcache_misses;

static uint32_t dc_hashfn(struct inode *dir, const char *name) {
    uint32_t h = (uint32_t)((uintptr_t)dir >> 4) * 2654435761u;
    for (; *name; name++) h = (h ^ (uint8_t)*name) * 16777619u;
    return h;
}
static struct dentry *dc_find(struct inode *dir, const char *name, uint32_t h) {
    if (!dc_hash[0].next) return nullptr;
    list_for_each(it, &dc_hash[h % DC_HASH]) {
        struct dentry *d = list_entry(it, struct dentry, hnode);
        if (d->hash == h && d->dir == dir && !strcmp(d->name, name)) return d;
    }
    return nullptr;
}
static void dc_drop(struct dentry *d) {
    list_del(&d->hnode); list_del(&d->lru);
    dc_count--;
    struct inode *i = d->ino, *dir = d->dir;
    kfree(d);
    iput(i); iput(dir);
}
static void dc_add(struct inode *dir, const char *name, uint32_t h, struct inode *i) {
    if (!dc_hash[0].next) for (int k = 0; k < DC_HASH; k++) list_init(&dc_hash[k]);
    size_t l = strlen(name);
    struct dentry *d = kmalloc(sizeof *d + l + 1);
    if (!d) return;
    memcpy(d->name, name, l + 1);
    d->dir = dir; d->ino = i; d->hash = h;
    iget(dir); iget(i);
    list_add(&dc_hash[h % DC_HASH], &d->hnode);
    list_add_tail(&dc_lru, &d->lru);
    if (++dc_count > DC_MAX) dc_drop(list_first(&dc_lru, struct dentry, lru));
}
static int dcache_lookup(struct inode *dir, const char *name, struct inode **out) {
    uint32_t h = dc_hashfn(dir, name);
    struct dentry *d = dc_find(dir, name, h);
    if (d) {
        list_del(&d->lru); list_add_tail(&dc_lru, &d->lru);
        if (!d->ino) { dcache_neg_hits++; return -ENOENT; }
        dcache_hits++;
        iget(d->ino); *out = d->ino;
        return 0;
    }
    dcache_misses++;
    int r = dir->iops->lookup(dir, name, out);
    if (!r) dc_add(dir, name, h, *out);
    else if (r == -ENOENT) dc_add(dir, name, h, nullptr);
    return r;
}
void dcache_forget(struct inode *dir, const char *name) {
    if (!dir->sb || !(dir->sb->flags & SB_DCACHE)) return;
    struct dentry *d = dc_find(dir, name, dc_hashfn(dir, name));
    if (d) dc_drop(d);
}
/* every entry in directory dir (it was removed), or of filesystem sb */
static void dcache_purge(struct inode *dir, struct super_block *sb) {
    if (!dc_hash[0].next) return;
    for (int k = 0; k < DC_HASH; k++)
        for (struct list_node *it = dc_hash[k].next, *nx; it != &dc_hash[k]; it = nx) {
            nx = it->next;
            struct dentry *d = list_entry(it, struct dentry, hnode);
            if (d->dir == dir || (sb && d->dir->sb == sb)) dc_drop(d);
        }
}

static int walk(struct inode *base, const char *path, bool follow_last, int depth, struct inode **out);
static int vfs_getcwd_l(struct inode *cwd, char *buf, size_t size);

/* ------------------------------------------------------------------ permissions (M31) */
int cred_inode_permission(const struct cred *c, struct inode *i, int mask) {
    mask &= MAY_READ | MAY_WRITE | MAY_EXEC;
    uint32_t m = i->mode, bits;
    if ((mask & MAY_WRITE) && i->sb && (i->sb->flags & SB_RDONLY) && (S_ISREG(m) || S_ISDIR(m) || S_ISLNK(m)))
        return -EROFS;
    int acl = 1;
    if (c->fsuid == i->uid) bits = m >> 6;
    else if (!(acl = acl_permission(c, i, mask))) return 0;
    else if (in_group(c, i->gid)) bits = m >> 3;
    else bits = m;
    if (acl > 0 && (bits & mask & 7) == (unsigned)mask) return 0;
    if (S_ISDIR(m)) {
        if (!(mask & MAY_WRITE) && cred_capable(c, CAP_DAC_READ_SEARCH)) return 0;
        if (cred_capable(c, CAP_DAC_OVERRIDE)) return 0;
    } else {
        if (mask == MAY_READ && cred_capable(c, CAP_DAC_READ_SEARCH)) return 0;
        /* root may execute only what somebody may execute */
        if ((!(mask & MAY_EXEC) || (m & 0111)) && cred_capable(c, CAP_DAC_OVERRIDE)) return 0;
    }
    return -EACCES;
}

int inode_permission(struct inode *i, int mask) { return cred_inode_permission(current_cred(), i, mask); }

bool inode_owner_or_capable(struct inode *i) {
    const struct cred *c = current_cred();
    return c->fsuid == i->uid || cred_capable(c, CAP_FOWNER);
}

void inode_init_owner(struct inode *i, struct inode *dir) {
    const struct cred *c = current_cred();
    i->uid = c->fsuid;
    if (dir && (dir->mode & S_ISGID)) {          /* BSD group semantics for setgid directories */
        i->gid = dir->gid;
        if (S_ISDIR(i->mode)) i->mode |= S_ISGID;
    } else i->gid = c->fsgid;
    if (!S_ISDIR(i->mode) && (i->mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP) &&
        !in_group(c, i->gid) && !cred_capable(c, CAP_FSETID))
        i->mode &= ~S_ISGID;
}

void file_remove_privs(struct file *f) {
    struct inode *i = f->inode;
    uint32_t m = __atomic_load_n(&i->mode, __ATOMIC_RELAXED);
    if (!S_ISREG(m) || !((m & S_ISUID) || (m & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP))) return;
    if (capable(CAP_FSETID)) return;
    uint32_t nm = m & ~S_ISUID;
    if ((m & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP)) nm &= ~S_ISGID;
    __atomic_compare_exchange_n(&i->mode, &m, nm, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    mark_inode_dirty(i);
}

int vfs_setattr_mode(struct inode *i, uint32_t mode) {
    if (i->sb && (i->sb->flags & SB_RDONLY)) return -EROFS;
    if (!inode_owner_or_capable(i)) return -EPERM;
    const struct cred *c = current_cred();
    mode &= 07777;
    if ((mode & S_ISGID) && !in_group(c, i->gid) && !cred_capable(c, CAP_FSETID)) mode &= ~S_ISGID;
    i->mode = (i->mode & S_IFMT) | mode;
    i->ctime = now_timespec();
    mark_inode_dirty(i);
    acl_chmod(i);
    return 0;
}

int vfs_setattr_owner(struct inode *i, uint32_t uid, uint32_t gid) {
    if (i->sb && (i->sb->flags & SB_RDONLY)) return -EROFS;
    const struct cred *c = current_cred();
    bool cap = cred_capable(c, CAP_CHOWN);
    if (uid != (uint32_t)-1 && uid != i->uid && !cap) return -EPERM;
    if (uid != (uint32_t)-1 && uid == i->uid && c->fsuid != i->uid && !cap) return -EPERM;
    if (gid != (uint32_t)-1 && gid != i->gid && !cap && (c->fsuid != i->uid || !in_group(c, gid))) return -EPERM;
    if (gid != (uint32_t)-1 && gid == i->gid && c->fsuid != i->uid && !cap) return -EPERM;
    if (uid == (uint32_t)-1 && gid == (uint32_t)-1) return 0;
    if (uid != (uint32_t)-1) i->uid = uid;
    if (gid != (uint32_t)-1) i->gid = gid;
    if (!S_ISDIR(i->mode)) {
        i->mode &= ~S_ISUID;
        if ((i->mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP)) i->mode &= ~S_ISGID;
    }
    i->ctime = now_timespec();
    mark_inode_dirty(i);
    return 0;
}

static int may_create(struct inode *dir) { return inode_permission(dir, MAY_WRITE | MAY_EXEC); }

/* unlink/rmdir/rename-away of victim from dir: W+X on dir, and the sticky-bit rule */
static int may_delete(struct inode *dir, struct inode *victim) {
    int r = inode_permission(dir, MAY_WRITE | MAY_EXEC);
    if (r) return r;
    if (victim && (dir->mode & S_ISVTX)) {
        const struct cred *c = current_cred();
        if (c->fsuid != victim->uid && c->fsuid != dir->uid && !cred_capable(c, CAP_FOWNER)) return -EPERM;
    }
    if (victim && (victim->mounted || victim->covered)) return -EBUSY;
    return 0;
}

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
    int r;
    if (dir->sb && (dir->sb->flags & SB_DCACHE)) r = dcache_lookup(dir, name, &c);
    else r = dir->iops->lookup(dir, name, &c);
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
        int r = S_ISDIR(cur->mode) ? inode_permission(cur, MAY_EXEC) : -ENOTDIR;
        if (!r) r = lookup_child(cur, name, &child);
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

static int vfs_lookup_at_l(struct inode *base, const char *path, bool follow, struct inode **out) {
    return walk(base, path, follow, 0, out);
}
static int vfs_lookup_l(const char *path, bool follow, struct inode **out) { return walk(nullptr, path, follow, 0, out); }

/* Resolve all but the last component. last receives the final name ("." for "/"). */
static int vfs_lookup_parent_at_l(struct inode *base, const char *path, struct inode **dir, char *last) {
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
    f->fops = S_ISCHR(ino->mode) ? chrdev_get(ino->rdev) : S_ISBLK(ino->mode) ? blkdev_fops_get(ino->rdev) : ino->fops;
    if (S_ISREG(ino->mode) && !(flags & O_PATH)) {
        f->counted = true;
        __atomic_add_fetch(&ino->i_nopen, 1, __ATOMIC_RELAXED);
        if ((flags & O_ACCMODE) != O_RDONLY) __atomic_add_fetch(&ino->i_nwrite, 1, __ATOMIC_RELAXED);
    }
    return f;
}

static int open_prepare(struct inode *base, const char *path, int flags, uint32_t mode, struct file **out) {
    struct inode *ino;
    int r = walk(base, path, !(flags & O_NOFOLLOW), 0, &ino);
    bool created = false;
    if (r == -ENOENT && (flags & O_CREAT)) {
        struct inode *dir; char last[256];
        r = vfs_lookup_parent_at_l(base, path, &dir, last);
        if (r) return r;
        if (!dir->iops->create) { iput(dir); return -EROFS; }
        if ((r = may_create(dir))) { iput(dir); return r; }
        created = true;
        r = dir->iops->create(dir, last, S_IFREG | acl_create_mode(dir, mode & 07777), 0, &ino);
        dcache_forget(dir, last);
        if (!r) { acl_inherit(dir, ino); fsnotify_dirent(dir, last, IN_CREATE, false, 0); }
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
    if (!created && !(flags & O_PATH)) {        /* M31: the creator may use what it just made */
        int acc = flags & O_ACCMODE;
        int mask = (acc == O_RDONLY || acc == O_RDWR ? MAY_READ : 0) | (acc == O_WRONLY || acc == O_RDWR ? MAY_WRITE : 0);
        if ((flags & O_TRUNC) && S_ISREG(ino->mode)) mask |= MAY_WRITE;
        if (acc == 3) mask = MAY_READ | MAY_WRITE;               /* Linux: 3 = ioctl-only, needs both */
        if (mask && (r = inode_permission(ino, mask))) {
            /* writes to device nodes/FIFOs on a read-only fs are fine */
            if (!(r == -EROFS && !S_ISREG(ino->mode) && !S_ISDIR(ino->mode))) { iput(ino); return r; }
        }
    }
    /* O_TRUNC: vfs_open_at, after breaking leases */
    struct file *f = file_open_inode(ino, flags);
    iput(ino);
    if (!f) return -ENOMEM;
    if (path[0] == '/') f->path = strdup(path);
    else {
        char *cwd = kmalloc(4096);
        int l = vfs_getcwd_l(base ? base : proc_cwd(), cwd, 4096);
        if (l > 0) {
            size_t pl = strlen(path);
            char *full = kmalloc(l + pl + 2);
            snprintf(full, l + pl + 2, "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", path);
            f->path = full;
        }
        kfree(cwd);
    }
    if ((S_ISCHR(ino->mode) || S_ISBLK(ino->mode)) && !f->fops) { vfs_close(f); return -ENXIO; }
    *out = f;
    return 0;
}

/* The walk/create run under the namespace mutex; lease breaks, O_TRUNC and the driver's ->open runs after it
 * is dropped (it may block, e.g. a FIFO, or create nodes itself, e.g. ptmx), under the BKL. */
int vfs_open_at(struct inode *base, const char *path, int flags, uint32_t mode, struct file **out) {
    struct file *f;
    vfs_ns_lock();
    int r = open_prepare(base, path, flags, mode, &f);
    vfs_ns_unlock();
    if (r) return r;
    if (f->counted && (r = lease_break(f->inode, flags))) { vfs_close(f); return r; }
    if (f->counted && (flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY && f->inode->iops->truncate)
        f->inode->iops->truncate(f->inode, 0);
    if (f->fops && f->fops->open && !(flags & O_PATH)) {
        bool took = !bkl_held();
        if (took) bkl_enter();
        r = f->fops->open(f->inode, f);
        if (took) bkl_exit();
        if (r) { f->fops = nullptr; vfs_close(f); return r; }
    }
    if (!(flags & O_PATH)) fsnotify_file(f, IN_OPEN);
    *out = f;
    return 0;
}

int vfs_open(const char *path, int flags, uint32_t mode, struct file **out) {
    return vfs_open_at(nullptr, path, flags, mode, out);
}

void locks_release_file(struct file *f);   /* locks.c */
void vfs_close(struct file *f) {
    if (__atomic_sub_fetch(&f->refcount, 1, __ATOMIC_ACQ_REL) > 0) return;
    /* the last reference may be dropped by a lock-free syscall: release under the BKL */
    bool took = !bkl_held();
    if (took) bkl_enter();
    if (f->inode && (S_ISREG(f->inode->mode) || S_ISDIR(f->inode->mode)) && !(f->flags & O_PATH))
        fsnotify_file(f, (f->flags & O_ACCMODE) != O_RDONLY ? IN_CLOSE_WRITE_ : IN_CLOSE_NOWRITE);
    locks_release_file(f);
    if (f->counted) {
        __atomic_sub_fetch(&f->inode->i_nopen, 1, __ATOMIC_RELAXED);
        if ((f->flags & O_ACCMODE) != O_RDONLY) __atomic_sub_fetch(&f->inode->i_nwrite, 1, __ATOMIC_RELAXED);
    }
    if (f->fops && f->fops->release) f->fops->release(f);
    iput(f->inode);
    kfree(f->path);
    kfree(f);
    if (took) bkl_exit();
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
    if (S_ISREG(f->inode->mode)) file_remove_privs(f);
    if ((f->flags & O_APPEND) && S_ISREG(f->inode->mode)) f->pos = f->inode->size;
    ssize_t r = f->fops->write(f, buf, n, &f->pos);
    if (r > 0 && S_ISREG(f->inode->mode)) fsnotify_file(f, IN_MODIFY_);
    return r;
}

ssize_t vfs_pread(struct file *f, void *buf, size_t n, off_t off) {
    if (!f->fops || !f->fops->read) return -EINVAL;
    return f->fops->read(f, buf, n, &off);
}

static int vfs_mknod_at_l(struct inode *base, const char *path, uint32_t mode, uint64_t rdev) {
    struct inode *dir, *ino; char last[256];
    int r = vfs_lookup_parent_at_l(base, path, &dir, last);
    if (r) return r;
    if (!dir->iops->create) { iput(dir); return -EROFS; }
    struct inode *ex;
    if (!lookup_child(dir, last, &ex)) { iput(ex); iput(dir); return -EEXIST; }
    if ((S_ISCHR(mode) || S_ISBLK(mode)) && !capable(CAP_MKNOD)) { iput(dir); return -EPERM; }
    if ((r = may_create(dir))) { iput(dir); return r; }
    if (mode & VFS_MODE_UMASK) mode = (mode & S_IFMT) | acl_create_mode(dir, mode & 07777);
    r = dir->iops->create(dir, last, mode, rdev, &ino);
    dcache_forget(dir, last);
    if (!r) acl_inherit(dir, ino);
    if (!r) { fsnotify_dirent(dir, last, IN_CREATE, S_ISDIR(mode), 0); iput(ino); }
    iput(dir);
    return r;
}

int vfs_mkdir_at(struct inode *base, const char *path, uint32_t mode) {
    return vfs_mknod_at(base, path, S_IFDIR | (mode & 07777) | VFS_MODE_UMASK, 0);
}

static int vfs_unlink_at_l(struct inode *base, const char *path, bool rmdir) {
    struct inode *dir; char last[256];
    int r = vfs_lookup_parent_at_l(base, path, &dir, last);
    if (r) return r;
    if (!strcmp(last, ".") || !strcmp(last, "..")) { iput(dir); return rmdir ? -EINVAL : -EISDIR; }
    struct inode *victim = nullptr;
    if (lookup_child(dir, last, &victim)) victim = nullptr;
    if (victim && (r = may_delete(dir, victim))) { iput(victim); iput(dir); return r; }
    if (!victim && (r = inode_permission(dir, MAY_WRITE | MAY_EXEC))) { iput(dir); return r; }
    dcache_forget(dir, last);
    if (victim && rmdir && dir->sb) dcache_purge(victim, nullptr);
    r = dir->iops->unlink ? dir->iops->unlink(dir, last, rmdir) : -EROFS;
    if (!r && victim && fsnotify_nwatches) {
        fsnotify_dirent(dir, last, IN_DELETE, S_ISDIR(victim->mode), 0);
        fsnotify_unlinked(victim);
    }
    if (victim) iput(victim);
    iput(dir);
    return r;
}

static int vfs_symlink_at_l(struct inode *base, const char *target, const char *path) {
    struct inode *dir; char last[256];
    int r = vfs_lookup_parent_at_l(base, path, &dir, last);
    if (r) return r;
    struct inode *ex;
    if (!lookup_child(dir, last, &ex)) { iput(ex); iput(dir); return -EEXIST; }
    if ((r = may_create(dir))) { iput(dir); return r; }
    r = dir->iops->symlink ? dir->iops->symlink(dir, last, target) : -EROFS;
    dcache_forget(dir, last);
    if (!r) fsnotify_dirent(dir, last, IN_CREATE, false, 0);
    iput(dir);
    return r;
}

/* protected_hardlinks (Linux default on distros): only link what you own or could read+write */
static int may_link(struct inode *src) {
    if (inode_owner_or_capable(src)) return 0;
    if (!S_ISREG(src->mode) || (src->mode & S_ISUID) || (src->mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP)) return -EPERM;
    return inode_permission(src, MAY_READ | MAY_WRITE) ? -EPERM : 0;
}

static int vfs_link_at_l(struct inode *ob, const char *opath, struct inode *nb, const char *npath, bool follow) {
    struct inode *src;
    int r = walk(ob, opath, follow, 0, &src);
    if (r) return r;
    if (S_ISDIR(src->mode)) { iput(src); return -EPERM; }
    struct inode *dir; char last[256];
    r = vfs_lookup_parent_at_l(nb, npath, &dir, last);
    if (r) { iput(src); return r; }
    struct inode *ex;
    if (!lookup_child(dir, last, &ex)) { iput(ex); r = -EEXIST; }
    else if ((r = may_create(dir)) || (r = may_link(src))) {}
    else r = dir->iops->link ? dir->iops->link(dir, last, src) : -EPERM;
    dcache_forget(dir, last);
    if (!r) { fsnotify_dirent(dir, last, IN_CREATE, false, 0); fsnotify_inode(src, IN_ATTRIB); }
    iput(dir); iput(src);
    return r;
}

static int vfs_rename_at_l(struct inode *ob, const char *opath, struct inode *nb, const char *npath) {
    struct inode *od, *nd; char ol[256], nl[256];
    int r = vfs_lookup_parent_at_l(ob, opath, &od, ol);
    if (r) return r;
    r = vfs_lookup_parent_at_l(nb, npath, &nd, nl);
    if (r) { iput(od); return r; }
    struct inode *moved = nullptr, *tgt = nullptr;
    if (lookup_child(od, ol, &moved)) moved = nullptr;
    if (!moved) r = -ENOENT;
    else if (od->iops != nd->iops || od->sb != nd->sb) r = -EXDEV;
    else if ((r = may_delete(od, moved))) {}
    else if (!lookup_child(nd, nl, &tgt) && tgt != moved && (r = may_delete(nd, tgt))) {}
    else if (!tgt && (r = may_create(nd))) {}
    else if (S_ISDIR(moved->mode) && nd != od && (r = inode_permission(moved, MAY_WRITE))) {}   /* ".." changes */
    if (tgt) { iput(tgt); tgt = nullptr; }
    if (r) {}
    else {
        if (od->sb) {
            struct inode *t;
            if (!lookup_child(nd, nl, &t)) { if (S_ISDIR(t->mode)) dcache_purge(t, nullptr); iput(t); }
        }
        dcache_forget(od, ol); dcache_forget(nd, nl);
        r = od->iops->rename ? od->iops->rename(od, ol, nd, nl) : -EROFS;
        dcache_forget(od, ol); dcache_forget(nd, nl);
    }
    if (!r && moved && fsnotify_nwatches) {
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

static int vfs_readlink_at_l(struct inode *base, const char *path, char *buf, size_t size) {
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

/* ---- mounts ---- */
struct mount {
    struct list_node node;
    struct inode *root, *mp;
    struct super_block *sb;       /* null for pseudo filesystems */
    char *src, *path;
    const char *type;
    uint64_t flags;
};
static struct list_node mounts = LIST_INIT(mounts);
#define MS_RDONLY 1
#define MS_REMOUNT 32
#define MS_BIND 4096
#define MS_MOVE 8192
#define MNT_FORCE 1
#define MNT_DETACH 2

static int vfs_getcwd_l(struct inode *cwd, char *buf, size_t size);
static void mount_record(struct inode *root, struct inode *mp, struct super_block *sb, const char *src,
                         const char *path, const char *type, uint64_t flags) {
    struct mount *m = kzalloc(sizeof *m);
    if (!m) return;
    m->root = root; m->mp = mp; m->sb = sb; m->type = type; m->flags = flags;
    m->src = strdup(src);
    char *buf = kmalloc(4096);
    if (buf && vfs_getcwd_l(root, buf, 4096) > 0) m->path = strdup(buf);
    else m->path = strdup(path);
    kfree(buf);
    list_add_tail(&mounts, &m->node);
}

static int attach(struct inode *mp, struct inode *root) {
    if (!S_ISDIR(mp->mode)) return -ENOTDIR;
    if (mp->mounted) return -EBUSY;
    mp->mounted = root;
    root->covered = mp;
    root->parent = mp->parent;
    return 0;
}

static int vfs_mount_l(const char *path, struct inode *root) {
    struct inode *mp;
    int r = vfs_lookup_l(path, true, &mp);
    if (r) return r;
    r = attach(mp, root);
    if (r) { iput(mp); return r; }
    mount_record(root, mp, nullptr, root->iops == nullptr ? "none" : "proc", path, "proc", 0);
    return 0;
}

static const struct fs_type *fs_types[8];
static int nfs_types;
void fs_register(const struct fs_type *t) { if (nfs_types < 8) fs_types[nfs_types++] = t; }
int vfs_proc_filesystems(char *buf, size_t max) {
    size_t n = snprintf(buf, max, "nodev\ttmpfs\nnodev\tproc\nnodev\tdevtmpfs\n");
    for (int k = 0; k < nfs_types && n < max; k++)
        n += snprintf(buf + n, max - n, "%s\t%s\n", fs_types[k]->needs_dev ? "" : "nodev", fs_types[k]->name);
    return (int)MIN(n, max);
}
int vfs_proc_mounts(char *buf, size_t max) {
    size_t n = 0;
    vfs_ns_lock();
    list_for_each(it, &mounts) {
        struct mount *m = list_entry(it, struct mount, node);
        if (n >= max) break;
        bool ro = m->sb ? (m->sb->flags & SB_RDONLY) : (m->flags & MS_RDONLY);
        n += snprintf(buf + n, max - n, "%s %s %s %s 0 0\n", m->src, m->path, m->type, ro ? "ro" : "rw");
    }
    vfs_ns_unlock();
    return (int)MIN(n, max);
}

static struct mount *mount_of_root(struct inode *root) {
    list_for_each(it, &mounts) {
        struct mount *m = list_entry(it, struct mount, node);
        if (m->root == root) return m;
    }
    return nullptr;
}

static int fs_mount_dev(const char *source, const char *type, uint64_t flags, const char *data, struct super_block **out) {
    const struct fs_type *t = nullptr;
    for (int k = 0; k < nfs_types; k++) if (!strcmp(fs_types[k]->name, type)) t = fs_types[k];
    if (!t) return -ENODEV;
    struct inode *di;
    int r = vfs_lookup_l(source, true, &di);
    if (r) return r;
    if (!S_ISBLK(di->mode)) { iput(di); return -ENOTBLK; }
    struct blkdev *bd = blk_get(di->rdev);
    iput(di);
    if (!bd) return -ENXIO;
    if (bd->mounted) return -EBUSY;
    r = t->mount(bd, flags & MS_RDONLY ? SB_RDONLY : 0, data, out);
    if (!r) bd->mounted = *out;
    return r;
}

static int vfs_do_mount_l(const char *source, const char *target, const char *type, uint64_t flags, const char *data) {
    struct inode *mp;
    int r = vfs_lookup_l(target, true, &mp);
    if (r) return r;
    if (flags & MS_REMOUNT) {
        struct mount *m = mount_of_root(mp);
        r = -EINVAL;
        if (m && m->sb && m->sb->ops->remount) r = m->sb->ops->remount(m->sb, flags & MS_RDONLY ? SB_RDONLY : 0);
        else if (m) { m->flags = flags; r = 0; }
        iput(mp);
        return r;
    }
    if (flags & (MS_BIND | MS_MOVE)) { iput(mp); return -EINVAL; }
    struct inode *root = nullptr;
    struct super_block *sb = nullptr;
    if (!type) r = -EINVAL;
    else if (!strcmp(type, "tmpfs")) { root = tmpfs_create_root(); root->parent = mp; }
    else if (!strcmp(type, "proc")) root = procfs_create_root();
    else if (!(r = fs_mount_dev(source, type, flags, data, &sb))) root = sb->root;
    if (!root) { iput(mp); return r ? r : -ENOMEM; }
    r = attach(mp, root);
    if (r) {
        iput(mp);
        if (sb) { sb->bdev->mounted = nullptr; sb->ops->put_super(sb); }
        return r;
    }
    mount_record(root, mp, sb, source && *source ? source : type, target,
                 sb ? sb->type : !strcmp(type, "tmpfs") ? "tmpfs" : "proc", flags);
    return 0;
}

/*
 * Evict every unused cached inode of sb. Cached directories pin their parent, so this
 * cascades up the tree: afterwards only inodes really in use (and their ancestors) remain.
 */
static void sb_prune_unused(struct super_block *sb) {
    uint64_t f = ic_lock(sb);
    for (;;) {
        struct inode *v = nullptr;
        for (int k = 0; k < ICACHE_HASH && !v; k++)
            list_for_each(it, &sb->ihash[k]) {
                struct inode *i = list_entry(it, struct inode, i_hash);
                if (i != sb->root && !__atomic_load_n(&i->refcount, __ATOMIC_ACQUIRE) && ic_detach(i, false)) { v = i; break; }
            }
        if (!v) break;
        ic_unhash(sb, v);
        ic_unlock(sb, f);
        sb->ops->evict_inode(v);
        f = ic_lock(sb);
    }
    ic_unlock(sb, f);
}

/* write back and drop every cached inode of sb (unmount) */
static int sb_shutdown(struct super_block *sb) {
    sb->flags |= SB_DYING;
    vfs_sync_all();
    for (int pass = 0; pass < 2; pass++) sb_prune_unused(sb);
    return 0;
}

static int vfs_do_umount_l(const char *target, int flags) {
    struct inode *root;
    int r = vfs_lookup_l(target, true, &root);
    if (r) return r;
    struct mount *m = mount_of_root(root);
    if (!m || m->root == vfs_root) { iput(root); return m ? -EBUSY : -EINVAL; }
    if (m->sb) { dcache_purge(nullptr, m->sb); sb_prune_unused(m->sb); }
    /* busy: references beyond the mount's own and ours (open files, cwds, inodes in use) */
    bool busy = root->refcount > 2;
    if (m->sb && !busy) {
        uint64_t f = ic_lock(m->sb);
        for (int k = 0; k < ICACHE_HASH && !busy; k++)
            list_for_each(it, &m->sb->ihash[k]) {
                struct inode *i = list_entry(it, struct inode, i_hash);
                if (i != root && __atomic_load_n(&i->refcount, __ATOMIC_ACQUIRE) > 0) { busy = true; break; }
            }
        ic_unlock(m->sb, f);
    }
    list_for_each(it, &mounts) {         /* something mounted inside it */
        struct mount *o = list_entry(it, struct mount, node);
        if (o != m && o->mp && (o->mp->sb == m->sb && m->sb)) busy = true;
    }
    iput(root);
    if (busy && !(flags & MNT_DETACH)) return -EBUSY;
    m->mp->mounted = nullptr;
    root->covered = nullptr;
    iput(m->mp);
    list_del(&m->node);
    if (m->sb && !busy) {
        struct super_block *sb = m->sb;
        sb_shutdown(sb);
        if (sb->ops->sync_fs) sb->ops->sync_fs(sb);
        blk_sync(sb->bdev);
        sb->bdev->mounted = nullptr;
        sb->ops->put_super(sb);
    }
    kfree(m->src); kfree(m->path); kfree(m);
    return 0;
}

/*
 * root=/dev/vdXN: mount the disk and make it "/". The boot tmpfs keeps /dev (device nodes,
 * /dev/shm, /dev/pts) and /tmp, which become mounts on the new root, as does /proc; the rest
 * of the initramfs stays reachable only by nobody (it remains as a fallback image in memory).
 */
int vfs_mount_root(const char *source, const char *type) {
    vfs_ns_lock();
    struct super_block *sb;
    int r = fs_mount_dev(source, type, 0, nullptr, &sb);
    if (r) { vfs_ns_unlock(); return r; }
    struct inode *nr = sb->root, *old = vfs_root;
    static const char *keep[] = { "dev", "proc", "tmp" };
    for (size_t k = 0; k < ARRAY_SIZE(keep); k++) {
        struct inode *src, *dst;
        if (lookup_child(old, keep[k], &src)) continue;
        if (lookup_child(nr, keep[k], &dst)) {
            if (nr->iops->create && !(sb->flags & SB_RDONLY) && !nr->iops->create(nr, keep[k], S_IFDIR | (k == 2 ? 01777 : 0755), 0, &dst)) dcache_forget(nr, keep[k]);
            else { iput(src); continue; }
        }
        /* src may itself be a mount root already (procfs): move that mount */
        struct inode *cov = src->covered;
        if (cov) cov->mounted = nullptr;
        src->covered = nullptr;
        if (!attach(dst, src)) {
            struct mount *m = mount_of_root(src);
            if (m) { if (m->mp) iput(m->mp); m->mp = dst; kfree(m->path); m->path = strdup(k == 0 ? "/dev" : k == 1 ? "/proc" : "/tmp"); }
            else mount_record(src, dst, nullptr, k == 0 ? "devtmpfs" : "tmpfs", k == 0 ? "/dev" : "/tmp", k == 0 ? "devtmpfs" : "tmpfs", 0);
        } else iput(dst);
        iput(src);
    }
    nr->parent = nr;
    vfs_root = nr;
    iget(nr);
    /* the new root entry first in /proc/mounts */
    struct mount *m = kzalloc(sizeof *m);
    m->root = nr; m->sb = sb; m->type = sb->type; m->src = strdup(source); m->path = strdup("/");
    list_add(&mounts, &m->node);
    list_for_each(it, &mounts) {
        struct mount *o = list_entry(it, struct mount, node);
        if (o != m && !strcmp(o->path, "/")) { list_del(&o->node); kfree(o->src); kfree(o->path); kfree(o); break; }
    }
    vfs_ns_unlock();
    pr_info("vfs: root is %s (%s)\n", source, sb->type);
    return 0;
}

/* ---- sync / statfs ---- */
int vfs_fsync(struct inode *i, bool data_only) {
    if (!i->sb) return 0;
    int r = 0;
    if (i->mapping) r = filemap_writeback(i->mapping, 0, UINT64_MAX);
    if (!data_only) mark_inode_dirty(i);
    if (i->sb->ops->sync_fs) i->sb->ops->sync_fs(i->sb);
    int e = blk_sync(i->sb->bdev);      /* metadata (indirect blocks, bitmaps, inode) + cache flush */
    return r ? r : e;
}

int vfs_sync_all(void) {
    for (int pass = 0; pass < 2; pass++) {
        list_for_each(it, &mounts) {
            struct mount *m = list_entry(it, struct mount, node);
            if (m->sb && m->sb->ops->sync_fs) m->sb->ops->sync_fs(m->sb);
        }
        writeback_all();     /* file data first, then the device caches it dirtied */
    }
    for (int k = 0; k < blk_count(); k++) {
        struct blkdev *d = blk_get_index(k);
        if (d->whole == d) blk_flush(d);
    }
    return 0;
}

/* reboot/poweroff: write everything back and leave disk filesystems clean (read-only) */
void vfs_shutdown(void) {
    vfs_sync_all();
    vfs_ns_lock();
    list_for_each(it, &mounts) {
        struct mount *m = list_entry(it, struct mount, node);
        if (m->sb && !(m->sb->flags & SB_RDONLY) && m->sb->ops->remount) m->sb->ops->remount(m->sb, SB_RDONLY);
    }
    vfs_ns_unlock();
}

int vfs_statfs(struct inode *i, struct kstatfs *st) {
    memset(st, 0, sizeof *st);
    if (i && i->sb && i->sb->ops->statfs) return i->sb->ops->statfs(i->sb, st);
    uint64_t free, total;
    pmm_stats(&free, &total);
    st->type = i && i->dev == 3 ? 0x9fa0 : 0x01021994;   /* PROC_SUPER_MAGIC / TMPFS_MAGIC */
    st->bsize = 4096;
    st->blocks = total; st->bfree = st->bavail = free;
    st->files = 1 << 20; st->ffree = 1 << 19;
    st->namelen = 255;
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

static int vfs_getcwd_l(struct inode *cwd, char *buf, size_t size) {
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
    mount_record(vfs_root, nullptr, nullptr, "rootfs", "/", "tmpfs", 0);
    pr_info("vfs: tmpfs root mounted\n");
}

/* ---- public entry points: take the namespace mutex ---- */
#define NS_WRAP(call) ({ vfs_ns_lock(); int __r = (call); vfs_ns_unlock(); __r; })
int vfs_lookup_at(struct inode *base, const char *path, bool follow, struct inode **out) { return NS_WRAP(vfs_lookup_at_l(base, path, follow, out)); }
int vfs_lookup(const char *path, bool follow, struct inode **out) { return NS_WRAP(vfs_lookup_l(path, follow, out)); }
int vfs_lookup_parent_at(struct inode *base, const char *path, struct inode **dir, char *last) { return NS_WRAP(vfs_lookup_parent_at_l(base, path, dir, last)); }
int vfs_mknod_at(struct inode *base, const char *path, uint32_t mode, uint64_t rdev) { return NS_WRAP(vfs_mknod_at_l(base, path, mode, rdev)); }
int vfs_unlink_at(struct inode *base, const char *path, bool rmdir) { return NS_WRAP(vfs_unlink_at_l(base, path, rmdir)); }
int vfs_symlink_at(struct inode *base, const char *target, const char *path) { return NS_WRAP(vfs_symlink_at_l(base, target, path)); }
int vfs_link_at(struct inode *ob, const char *opath, struct inode *nb, const char *npath, bool follow) { return NS_WRAP(vfs_link_at_l(ob, opath, nb, npath, follow)); }
int vfs_rename_at(struct inode *ob, const char *opath, struct inode *nb, const char *npath) { return NS_WRAP(vfs_rename_at_l(ob, opath, nb, npath)); }
int vfs_readlink_at(struct inode *base, const char *path, char *buf, size_t size) { return NS_WRAP(vfs_readlink_at_l(base, path, buf, size)); }
int vfs_mount(const char *path, struct inode *root) { return NS_WRAP(vfs_mount_l(path, root)); }
int vfs_do_mount(const char *source, const char *target, const char *type, uint64_t flags, const char *data) { return NS_WRAP(vfs_do_mount_l(source, target, type, flags, data)); }
int vfs_do_umount(const char *target, int flags) { return NS_WRAP(vfs_do_umount_l(target, flags)); }
int vfs_getcwd(struct inode *cwd, char *buf, size_t size) { return NS_WRAP(vfs_getcwd_l(cwd, buf, size)); }
