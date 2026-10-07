/* tmpfs: in-memory filesystem (root filesystem of 9os). */
#include <kernel/vfs.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/list.h>
#include <kernel/mutex.h>
#include <kernel/mm.h>
#include <kernel/pagecache.h>

/* Entries are kept in insertion order with a per-directory increasing cookie; readdir positions
 * are cookies, so unlinking or renaming entries during a readdir never skips survivors. */
struct dirent_t { struct list_node node; struct inode *ino; uint64_t cookie; char name[]; };
struct tdir { struct list_node entries; uint64_t count; uint64_t next_cookie; };
/*
 * Regular files keep their data in the unified page cache (M30, mm/filemap.c). tf->lock
 * (mutex) serialises writers/truncate; readers and the fault path rely on page references and
 * the mapping lock. Files unpacked from the initramfs are backed by the (permanently reserved)
 * archive image: pages are filled on demand (inline, even from the fault path) and clean
 * backed pages are reclaimable. Written pages and pages without backing are dirty forever
 * (tmpfs has no ->writepage: there is no swap), so they are never reclaimed.
 */
struct tfile {
    struct mutex lock;
    struct address_space as;
    struct inode *inode;
    const uint8_t *backing; size_t backing_len;
};
static const struct lock_class tfile_class = { "tmpfs_inode", LR_MUTEX_INODE, false };

static int t_readpage(struct address_space *m, struct page *pg) {
    struct tfile *f = m->host->priv;
    uint8_t *d = PHYS_TO_VIRT(page_to_phys(pg));
    uint64_t off = (uint64_t)pg->index * PAGE_SIZE;
    size_t n = off < f->backing_len ? MIN((size_t)PAGE_SIZE, f->backing_len - off) : 0;
    if (n) memcpy(d, f->backing + off, n);
    if (n < PAGE_SIZE) memset(d + n, 0, PAGE_SIZE - n);
    if (n) __atomic_fetch_add(&pagecache_filled, 1, __ATOMIC_RELAXED);
    else pagecache_mark_dirty(pg);       /* no backing: the only copy of the data */
    filemap_read_done(pg, 0);
    return 0;
}
static const struct aspace_ops tmpfs_aops = { .readpage = t_readpage };

#define copy_out pc_copy_out
#define copy_in pc_copy_in

static const struct inode_ops tmpfs_iops;
static const struct file_ops tmpfs_fops, tmpfs_dir_fops;

static struct dirent_t *dir_find(struct inode *dir, const char *name) {
    struct tdir *d = dir->priv;
    list_for_each(it, &d->entries) {
        struct dirent_t *e = list_entry(it, struct dirent_t, node);
        if (!strcmp(e->name, name)) return e;
    }
    return nullptr;
}

static int dir_add(struct inode *dir, const char *name, struct inode *ino) {
    size_t l = strlen(name);
    struct dirent_t *e = kmalloc(sizeof *e + l + 1);
    if (!e) return -ENOMEM;
    memcpy(e->name, name, l + 1);
    e->ino = ino;
    struct tdir *d = dir->priv;
    e->cookie = d->next_cookie++;
    list_add_tail(&d->entries, &e->node);
    d->count++;
    ino->nlink++;
    dir->mtime = dir->ctime = now_timespec();
    return 0;
}

static struct inode *tmpfs_new(uint32_t mode, uint64_t rdev) {
    struct inode *i = inode_alloc(mode);
    if (!i) return nullptr;
    i->iops = &tmpfs_iops;
    i->rdev = rdev;
    if (S_ISDIR(mode)) {
        struct tdir *d = kzalloc(sizeof *d);
        list_init(&d->entries);
        i->priv = d;
        i->fops = &tmpfs_dir_fops;
        i->nlink = 1;          /* "." */
    } else if (S_ISREG(mode)) {
        struct tfile *tf = kzalloc(sizeof(struct tfile));
        if (tf) {
            mutex_init(&tf->lock, &tfile_class);
            mapping_init(&tf->as, i, &tmpfs_aops);
            tf->as.atomic_fill = true;
            tf->inode = i;
            i->mapping = &tf->as;
        }
        i->priv = tf;
        i->fops = &tmpfs_fops;
    }
    return i;
}

static int t_lookup(struct inode *dir, const char *name, struct inode **out) {
    struct dirent_t *e = dir_find(dir, name);
    if (!e) return -ENOENT;
    iget(e->ino);
    *out = e->ino;
    return 0;
}

static int t_create(struct inode *dir, const char *name, uint32_t mode, uint64_t rdev, struct inode **out) {
    if (dir_find(dir, name)) return -EEXIST;
    struct inode *i = tmpfs_new(mode, rdev);
    if (!i) return -ENOMEM;
    inode_init_owner(i, dir);
    if (S_ISDIR(mode)) { i->parent = dir; dir->nlink++; }
    int r = dir_add(dir, name, i);
    if (r) { iput(i); return r; }
    *out = i;
    return 0;
}

static void t_evict(struct inode *i) {
    if (S_ISREG(i->mode)) {
        struct tfile *f = i->priv;
        mapping_truncate(&f->as, 0);
        kfree(f);
    } else if (S_ISLNK(i->mode) || S_ISDIR(i->mode)) {
        kfree(i->priv);
    }
}

static int t_unlink(struct inode *dir, const char *name, bool rmdir) {
    struct dirent_t *e = dir_find(dir, name);
    if (!e) return -ENOENT;
    struct inode *i = e->ino;
    if (rmdir) {
        if (!S_ISDIR(i->mode)) return -ENOTDIR;
        if (((struct tdir *)i->priv)->count) return -ENOTEMPTY;
        if (i->mounted) return -EBUSY;
        dir->nlink--;
        i->nlink = 0;
    } else {
        if (S_ISDIR(i->mode)) return -EISDIR;
        i->nlink--;
    }
    list_del(&e->node);
    ((struct tdir *)dir->priv)->count--;
    kfree(e);
    dir->mtime = dir->ctime = now_timespec();
    iget(i); iput(i);       /* frees the inode if nothing references it */
    return 0;
}

static int t_symlink(struct inode *dir, const char *name, const char *target) {
    struct inode *i = tmpfs_new(S_IFLNK | 0777, 0);
    if (!i) return -ENOMEM;
    inode_init_owner(i, dir);
    i->priv = strdup(target);
    i->size = strlen(target);
    int r = dir_add(dir, name, i);
    iput(i);
    return r;
}

static int t_readlink(struct inode *i, char *buf, size_t size) {
    size_t l = MIN(i->size, size);
    memcpy(buf, i->priv, l);
    return (int)l;
}

static int t_link(struct inode *dir, const char *name, struct inode *target) {
    if (target->iops != &tmpfs_iops) return -EXDEV;
    return dir_add(dir, name, target);
}

static bool is_ancestor(struct inode *a, struct inode *d) {
    for (int n = 0; d && n < 4096; n++) {
        if (d == a) return true;
        if (d->parent == d) break;
        d = d->parent;
    }
    return false;
}

static int t_rename(struct inode *od, const char *on, struct inode *nd, const char *nn) {
    struct dirent_t *e = dir_find(od, on);
    if (!e) return -ENOENT;
    struct inode *i = e->ino;
    if (od == nd && !strcmp(on, nn)) return 0;
    if (S_ISDIR(i->mode) && is_ancestor(i, nd)) return -EINVAL;
    struct dirent_t *t = dir_find(nd, nn);
    if (t) {
        if (S_ISDIR(t->ino->mode) != S_ISDIR(i->mode)) return S_ISDIR(i->mode) ? -ENOTDIR : -EISDIR;
        int r = t_unlink(nd, nn, S_ISDIR(t->ino->mode));
        if (r) return r;
    }
    iget(i);
    list_del(&e->node);
    ((struct tdir *)od->priv)->count--;
    kfree(e);
    i->nlink--;
    int r = dir_add(nd, nn, i);
    if (S_ISDIR(i->mode) && od != nd) { od->nlink--; nd->nlink++; i->parent = nd; }
    iput(i);
    return r;
}

static int t_truncate_locked(struct inode *i, uint64_t size);
static int t_truncate(struct inode *i, uint64_t size) {
    if (!S_ISREG(i->mode)) return -EINVAL;
    struct tfile *f = i->priv;
    mutex_lock(&f->lock);
    int r = t_truncate_locked(i, size);
    mutex_unlock(&f->lock);
    return r;
}
static int t_truncate_locked(struct inode *i, uint64_t size) {
    struct tfile *f = i->priv;
    if (size < i->size) {
        mapping_truncate(&f->as, (size + PAGE_SIZE - 1) / PAGE_SIZE);
        filemap_zero_tail(&f->as, size);
        if (f->backing_len > size) f->backing_len = size;   /* unfilled tail pages read as zero */
    }
    i->size = size;
    i->mtime = i->ctime = now_timespec();
    return 0;
}

static int t_iterate(struct inode *dir, uint64_t *pos, filldir_t fill, void *ctx) {
    struct tdir *d = dir->priv;
    if (*pos == 0) { if (fill(ctx, ".", 1, dir->ino, 4)) return 0; (*pos)++; }
    if (*pos == 1) {
        struct inode *p = dir->parent ? dir->parent : dir;
        if (fill(ctx, "..", 2, p->ino, 4)) return 0;
        (*pos)++;
    }
    list_for_each(it, &d->entries) {
        struct dirent_t *e = list_entry(it, struct dirent_t, node);
        if (e->cookie < *pos - 2) continue;
        unsigned type = (e->ino->mode & S_IFMT) >> 12;
        if (fill(ctx, e->name, strlen(e->name), e->ino->ino, type)) return 0;
        *pos = e->cookie + 3;
    }
    return 0;
}

static ssize_t t_read(struct file *fl, void *buf, size_t n, off_t *off) {
    struct inode *i = fl->inode;
    struct tfile *f = i->priv;
    if (*off < 0) return -EINVAL;
    ssize_t r = filemap_read(&f->as, __atomic_load_n(&i->size, __ATOMIC_ACQUIRE), buf, n, *off);
    if (r > 0) *off += r;
    return r;
}

static ssize_t t_write(struct file *fl, const void *buf, size_t n, off_t *off) {
    struct inode *i = fl->inode;
    struct tfile *f = i->priv;
    mutex_lock(&f->lock);
    if (fl->flags & O_APPEND) *off = i->size;
    if (*off < 0) { mutex_unlock(&f->lock); return -EINVAL; }
    uint64_t size = i->size;
    ssize_t r = filemap_write(&f->as, &size, buf, n, *off);
    if (r > 0) {
        *off += r;
        __atomic_store_n(&i->size, size, __ATOMIC_RELEASE);
        i->mtime = i->ctime = now_timespec();
    }
    mutex_unlock(&f->lock);
    return r == -ENOMEM ? -ENOSPC : r;
}

static unsigned t_poll(struct file *f) { return POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM; }

static const struct inode_ops tmpfs_iops = {
    .lookup = t_lookup, .create = t_create, .unlink = t_unlink, .symlink = t_symlink,
    .readlink = t_readlink, .link = t_link, .rename = t_rename, .truncate = t_truncate,
    .iterate = t_iterate, .evict = t_evict,
};
/* file_ops.fault_page: runs under mm->lock (atomic). Returns the page with a reference. */
static int t_fault_page(struct inode *ino, uint64_t pgoff, bool shared, paddr_t *out) {
    struct tfile *f = ino->priv;
    return filemap_fault(&f->as, pgoff, __atomic_load_n(&ino->size, __ATOMIC_RELAXED), shared, out);
}

/* lazily filled from an initramfs image that stays mapped (fs/initramfs.c) */
void tmpfs_set_backing(struct inode *i, const void *data, size_t len) {
    struct tfile *f = i->priv;
    mutex_lock(&f->lock);
    t_truncate_locked(i, 0);
    f->backing = data;
    f->backing_len = len;
    i->size = len;
    mutex_unlock(&f->lock);
}

static const struct file_ops tmpfs_fops = { .nobkl = true, .read = t_read, .write = t_write, .poll = t_poll, .fault_page = t_fault_page };

/* unlinked regular file (memfd_create, O_TMPFILE) */
struct inode *tmpfs_create_anon(uint32_t mode) { return tmpfs_new(mode, 0); }
static const struct file_ops tmpfs_dir_fops = { .poll = t_poll };

struct inode *tmpfs_create_root(void) {
    struct inode *r = tmpfs_new(S_IFDIR | 0755, 0);
    r->nlink = 2;
    return r;
}
