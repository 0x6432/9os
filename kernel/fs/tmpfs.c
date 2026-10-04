/* tmpfs: in-memory filesystem (root filesystem of 9os). */
#include <kernel/vfs.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/list.h>

struct dirent_t { struct list_node node; struct inode *ino; char name[]; };
struct tdir { struct list_node entries; uint64_t count; };
struct tfile { paddr_t *pages; size_t npages; };

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
        i->priv = kzalloc(sizeof(struct tfile));
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
    if (S_ISDIR(mode)) { i->parent = dir; dir->nlink++; }
    int r = dir_add(dir, name, i);
    if (r) { iput(i); return r; }
    *out = i;
    return 0;
}

static void t_evict(struct inode *i) {
    if (S_ISREG(i->mode)) {
        struct tfile *f = i->priv;
        for (size_t k = 0; k < f->npages; k++) if (f->pages[k]) page_put_pa(f->pages[k]);
        kfree(f->pages);
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

static int ensure_pages(struct tfile *f, size_t n) {
    if (n <= f->npages) return 0;
    size_t cap = MAX(n, f->npages * 2);
    paddr_t *np = kzalloc(cap * sizeof(paddr_t));
    if (!np) return -ENOMEM;
    if (f->pages) { memcpy(np, f->pages, f->npages * sizeof(paddr_t)); kfree(f->pages); }
    f->pages = np;
    f->npages = cap;
    return 0;
}

static int t_truncate(struct inode *i, uint64_t size) {
    if (!S_ISREG(i->mode)) return -EINVAL;
    struct tfile *f = i->priv;
    size_t need = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (size < i->size) {
        for (size_t k = need; k < f->npages; k++)
            if (f->pages[k]) { page_put_pa(f->pages[k]); f->pages[k] = 0; }
        if (size % PAGE_SIZE && need && f->pages[need - 1])
            memset((uint8_t *)PHYS_TO_VIRT(f->pages[need - 1]) + size % PAGE_SIZE, 0, PAGE_SIZE - size % PAGE_SIZE);
    } else if (ensure_pages(f, need)) return -ENOMEM;
    i->size = size;
    i->mtime = i->ctime = now_timespec();
    return 0;
}

static int t_iterate(struct inode *dir, uint64_t *pos, filldir_t fill, void *ctx) {
    struct tdir *d = dir->priv;
    uint64_t idx = 0;
    if (*pos == 0) { if (fill(ctx, ".", 1, dir->ino, 4)) return 0; (*pos)++; }
    if (*pos == 1) {
        struct inode *p = dir->parent ? dir->parent : dir;
        if (fill(ctx, "..", 2, p->ino, 4)) return 0;
        (*pos)++;
    }
    list_for_each(it, &d->entries) {
        if (idx++ < *pos - 2) continue;
        struct dirent_t *e = list_entry(it, struct dirent_t, node);
        unsigned type = (e->ino->mode & S_IFMT) >> 12;
        if (fill(ctx, e->name, strlen(e->name), e->ino->ino, type)) return 0;
        (*pos)++;
    }
    return 0;
}

static ssize_t t_read(struct file *fl, void *buf, size_t n, off_t *off) {
    struct inode *i = fl->inode;
    struct tfile *f = i->priv;
    if (*off < 0) return -EINVAL;
    if ((uint64_t)*off >= i->size) return 0;
    n = MIN(n, i->size - *off);
    size_t done = 0;
    while (done < n) {
        uint64_t pos = *off + done;
        size_t pg = pos / PAGE_SIZE, po = pos % PAGE_SIZE, chunk = MIN(n - done, PAGE_SIZE - po);
        if (pg < f->npages && f->pages[pg]) memcpy((uint8_t *)buf + done, (uint8_t *)PHYS_TO_VIRT(f->pages[pg]) + po, chunk);
        else memset((uint8_t *)buf + done, 0, chunk);
        done += chunk;
    }
    *off += done;
    return done;
}

static ssize_t t_write(struct file *fl, const void *buf, size_t n, off_t *off) {
    struct inode *i = fl->inode;
    struct tfile *f = i->priv;
    if (*off < 0) return -EINVAL;
    uint64_t end = *off + n;
    if (ensure_pages(f, (end + PAGE_SIZE - 1) / PAGE_SIZE)) return -ENOSPC;
    size_t done = 0;
    while (done < n) {
        uint64_t pos = *off + done;
        size_t pg = pos / PAGE_SIZE, po = pos % PAGE_SIZE, chunk = MIN(n - done, PAGE_SIZE - po);
        if (!f->pages[pg]) {
            f->pages[pg] = pmm_alloc_zeroed(0);
            if (!f->pages[pg]) break;
        }
        memcpy((uint8_t *)PHYS_TO_VIRT(f->pages[pg]) + po, (const uint8_t *)buf + done, chunk);
        done += chunk;
    }
    if (!done && n) return -ENOSPC;
    *off += done;
    if ((uint64_t)*off > i->size) i->size = *off;
    i->mtime = i->ctime = now_timespec();
    return done;
}

static unsigned t_poll(struct file *f) { return POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM; }

static const struct inode_ops tmpfs_iops = {
    .lookup = t_lookup, .create = t_create, .unlink = t_unlink, .symlink = t_symlink,
    .readlink = t_readlink, .link = t_link, .rename = t_rename, .truncate = t_truncate,
    .iterate = t_iterate, .evict = t_evict,
};
/* MAP_SHARED: hand out the page cache page itself (allocating holes). */
static int t_mmap_page(struct file *fl, uint64_t pgoff, paddr_t *pa) {
    struct tfile *f = fl->inode->priv;
    if (ensure_pages(f, pgoff + 1)) return -ENOMEM;
    if (!f->pages[pgoff] && !(f->pages[pgoff] = pmm_alloc_zeroed(0))) return -ENOMEM;
    *pa = f->pages[pgoff];
    return 0;
}
static const struct file_ops tmpfs_fops = { .read = t_read, .write = t_write, .poll = t_poll, .mmap_page = t_mmap_page };

/* unlinked regular file (memfd_create, O_TMPFILE) */
struct inode *tmpfs_create_anon(uint32_t mode) { return tmpfs_new(mode, 0); }
static const struct file_ops tmpfs_dir_fops = { .poll = t_poll };

struct inode *tmpfs_create_root(void) {
    struct inode *r = tmpfs_new(S_IFDIR | 0755, 0);
    r->nlink = 2;
    return r;
}
