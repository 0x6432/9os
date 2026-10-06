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

/* Entries are kept in insertion order with a per-directory increasing cookie; readdir positions
 * are cookies, so unlinking or renaming entries during a readdir never skips survivors. */
struct dirent_t { struct list_node node; struct inode *ino; uint64_t cookie; char name[]; };
struct tdir { struct list_node entries; uint64_t count; uint64_t next_cookie; };
/*
 * Regular files keep their data in a page array (the page cache; M26). Locking:
 *  - tf->lock (mutex) serialises read/write/truncate/reclaim of the file data and size;
 *  - tf->pglock (IRQ-off spinlock) guards the page-array entries: they are only changed with
 *    it held, so the fault path (file_ops.fault_page, under mm->lock) can look up and fill
 *    pages without the mutex. The array itself is only reallocated with both held.
 * Files unpacked from the initramfs are backed by the (permanently reserved) archive image:
 * pages are filled on demand, and clean backed pages sit on a global LRU from which
 * tmpfs_reclaim() drops them under memory pressure after unmapping them through the inode's
 * i_mmap reverse map. Written (dirty) and unbacked pages are never reclaimed (no swap).
 */
struct tfile {
    struct mutex lock;
    spinlock_t pglock;
    paddr_t *pages; size_t npages;
    struct inode *inode;
    const uint8_t *backing; size_t backing_len;
};
static const struct lock_class tfile_class = { "tmpfs_inode", LR_MUTEX_INODE, false };
static const struct lock_class pglock_class = { "tmpfs_pages", LR_PAGECACHE, false };
static const struct lock_class lru_class = { "lru", LR_LRU, false };

static struct list_node lru = LIST_INIT(lru);
static spinlock_t lru_lock = SPINLOCK_INIT_CLASS(&lru_class);
uint64_t pagecache_pages, pagecache_lru_pages, pagecache_filled, pagecache_reclaimed;

static uint64_t pg_lock(struct tfile *f) { uint64_t fl = arch_irq_save(); spin_lock_ipi(&f->pglock); return fl; }
static void pg_unlock(struct tfile *f, uint64_t fl) { spin_unlock(&f->pglock); arch_irq_restore(fl); }

static void lru_add(struct page *pg) {
    uint64_t f = arch_irq_save();
    spin_lock_ipi(&lru_lock);
    if (!page_uflag_test(pg, PGU_LRU | PGU_DIRTY)) {
        page_uflag_set(pg, PGU_LRU);
        list_add_tail(&lru, &pg->node);
        pagecache_lru_pages++;
    }
    spin_unlock(&lru_lock);
    arch_irq_restore(f);
}
static void lru_del_locked(struct page *pg) {
    if (!page_uflag_test(pg, PGU_LRU)) return;
    list_del(&pg->node);
    page_uflag_clear(pg, PGU_LRU);
    pagecache_lru_pages--;
}
void pagecache_mark_dirty(struct page *pg) {
    if (page_uflag_test(pg, PGU_DIRTY)) return;
    uint64_t f = arch_irq_save();
    spin_lock_ipi(&lru_lock);
    page_uflag_set(pg, PGU_DIRTY);
    lru_del_locked(pg);
    spin_unlock(&lru_lock);
    arch_irq_restore(f);
}
void pagecache_mark_referenced(struct page *pg) {
    if (!page_uflag_test(pg, PGU_REFERENCED)) page_uflag_set(pg, PGU_REFERENCED);
}

/* new page-cache page for index idx, filled from the backing image or zeroed; caller holds
 * pglock (and stores it in the array) */
static paddr_t cache_fill(struct tfile *f, uint64_t idx) {
    paddr_t pa = pmm_alloc_pages(0);
    if (!pa) return 0;
    uint8_t *d = PHYS_TO_VIRT(pa);
    uint64_t off = idx * PAGE_SIZE;
    size_t n = off < f->backing_len ? MIN((size_t)PAGE_SIZE, f->backing_len - off) : 0;
    if (n) memcpy(d, f->backing + off, n);
    if (n < PAGE_SIZE) memset(d + n, 0, PAGE_SIZE - n);
    struct page *pg = phys_to_page(pa);
    pg->mapping = f->inode;
    pg->index = (uint32_t)idx;
    page_uflag_set(pg, PGU_CACHE | PGU_REFERENCED);
    __atomic_fetch_add(&pagecache_pages, 1, __ATOMIC_RELAXED);
    if (n) { __atomic_fetch_add(&pagecache_filled, 1, __ATOMIC_RELAXED); lru_add(pg); }
    else page_uflag_set(pg, PGU_DIRTY);       /* no backing: only copy of the data */
    return pa;
}

/* drop the cache's reference on a page leaving the cache; caller holds pglock */
static void cache_drop(paddr_t pa) {
    struct page *pg = phys_to_page(pa);
    uint64_t f = arch_irq_save();
    spin_lock_ipi(&lru_lock);
    lru_del_locked(pg);
    spin_unlock(&lru_lock);
    arch_irq_restore(f);
    page_uflag_clear(pg, PGU_CACHE);
    pg->mapping = nullptr;
    __atomic_fetch_sub(&pagecache_pages, 1, __ATOMIC_RELAXED);
    page_put(pg);
}

static int copy_out(void *dst, const void *src, size_t n) {
    if ((vaddr_t)dst >= USER_TOP) { memcpy(dst, src, n); return 0; }
    return copy_to_user(dst, src, n);
}
static int copy_in(void *dst, const void *src, size_t n) {
    if ((vaddr_t)src >= USER_TOP) { memcpy(dst, src, n); return 0; }
    return copy_from_user(dst, src, n);
}

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
        if (tf) { mutex_init(&tf->lock, &tfile_class); spin_lock_init_class(&tf->pglock, &pglock_class); tf->inode = i; }
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
    if (S_ISDIR(mode)) { i->parent = dir; dir->nlink++; }
    int r = dir_add(dir, name, i);
    if (r) { iput(i); return r; }
    *out = i;
    return 0;
}

static void t_evict(struct inode *i) {
    if (S_ISREG(i->mode)) {
        struct tfile *f = i->priv;
        mutex_lock(&f->lock);                 /* reclaim may hold it (trylock under the LRU lock) */
        uint64_t fl = pg_lock(f);
        for (size_t k = 0; k < f->npages; k++) if (f->pages[k]) { cache_drop(f->pages[k]); f->pages[k] = 0; }
        pg_unlock(f, fl);
        mutex_unlock(&f->lock);
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
    uint64_t fl = pg_lock(f);
    paddr_t *old = f->pages;
    if (old) memcpy(np, old, f->npages * sizeof(paddr_t));
    f->pages = np;
    f->npages = cap;
    pg_unlock(f, fl);
    kfree(old);
    return 0;
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
    size_t need = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (size < i->size) {
        uint64_t fl = pg_lock(f);
        for (size_t k = need; k < f->npages; k++)
            if (f->pages[k]) { cache_drop(f->pages[k]); f->pages[k] = 0; }
        if (size % PAGE_SIZE && need && f->pages[need - 1]) {
            pagecache_mark_dirty(phys_to_page(f->pages[need - 1]));
            memset((uint8_t *)PHYS_TO_VIRT(f->pages[need - 1]) + size % PAGE_SIZE, 0, PAGE_SIZE - size % PAGE_SIZE);
        }
        if (f->backing_len > size) f->backing_len = size;   /* unfilled tail pages read as zero */
        pg_unlock(f, fl);
    } else if (ensure_pages(f, need)) return -ENOMEM;
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
    mutex_lock(&f->lock);
    if ((uint64_t)*off >= i->size) { mutex_unlock(&f->lock); return 0; }
    n = MIN(n, i->size - *off);
    size_t done = 0;
    static const uint8_t zero[256];
    int err = 0;
    while (done < n) {
        uint64_t pos = *off + done;
        size_t pg = pos / PAGE_SIZE, po = pos % PAGE_SIZE, chunk = MIN(n - done, PAGE_SIZE - po);
        paddr_t pa = pg < f->npages ? __atomic_load_n(&f->pages[pg], __ATOMIC_ACQUIRE) : 0;
        if (pa) {
            pagecache_mark_referenced(phys_to_page(pa));
            err = copy_out((uint8_t *)buf + done, (uint8_t *)PHYS_TO_VIRT(pa) + po, chunk);
        } else if (pos < f->backing_len) {       /* not cached: straight from the image */
            chunk = MIN(chunk, f->backing_len - pos);
            err = copy_out((uint8_t *)buf + done, f->backing + pos, chunk);
        } else {
            chunk = MIN(chunk, sizeof zero);
            err = copy_out((uint8_t *)buf + done, zero, chunk);
        }
        if (err) break;
        done += chunk;
    }
    *off += done;
    mutex_unlock(&f->lock);
    return !done && err ? -EFAULT : (ssize_t)done;
}

static ssize_t t_write(struct file *fl, const void *buf, size_t n, off_t *off) {
    struct inode *i = fl->inode;
    struct tfile *f = i->priv;
    mutex_lock(&f->lock);
    if (fl->flags & O_APPEND) *off = i->size;
    if (*off < 0) { mutex_unlock(&f->lock); return -EINVAL; }
    uint64_t end = *off + n;
    if (ensure_pages(f, (end + PAGE_SIZE - 1) / PAGE_SIZE)) { mutex_unlock(&f->lock); return -ENOSPC; }
    size_t done = 0;
    int err = 0;
    while (done < n) {
        uint64_t pos = *off + done;
        size_t pg = pos / PAGE_SIZE, po = pos % PAGE_SIZE, chunk = MIN(n - done, PAGE_SIZE - po);
        uint64_t fl = pg_lock(f);
        paddr_t pa = f->pages[pg];
        if (!pa) pa = f->pages[pg] = cache_fill(f, pg);
        pg_unlock(f, fl);
        if (!pa) break;
        pagecache_mark_dirty(phys_to_page(pa));
        if ((err = copy_in((uint8_t *)PHYS_TO_VIRT(pa) + po, (const uint8_t *)buf + done, chunk))) break;
        done += chunk;
    }
    if (!done && n) { mutex_unlock(&f->lock); return err ? -EFAULT : -ENOSPC; }
    *off += done;
    if ((uint64_t)*off > i->size) i->size = *off;
    i->mtime = i->ctime = now_timespec();
    mutex_unlock(&f->lock);
    return done;
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
    if (pgoff >= (__atomic_load_n(&ino->size, __ATOMIC_RELAXED) + PAGE_SIZE - 1) / PAGE_SIZE) return -ENXIO;
    uint64_t fl = pg_lock(f);
    int r = 0;
    paddr_t pa = pgoff < f->npages ? f->pages[pgoff] : 0;
    if (pgoff >= f->npages) r = -ENXIO;
    else if (!pa && !(pa = f->pages[pgoff] = cache_fill(f, pgoff))) r = -ENOMEM;
    else {
        struct page *pg = phys_to_page(pa);
        page_ref_inc(pg);
        pagecache_mark_referenced(pg);
        *out = pa;
    }
    pg_unlock(f, fl);
    return r;
}

/* lazily filled from an initramfs image that stays mapped (fs/initramfs.c) */
void tmpfs_set_backing(struct inode *i, const void *data, size_t len) {
    struct tfile *f = i->priv;
    mutex_lock(&f->lock);
    t_truncate_locked(i, 0);
    if (!ensure_pages(f, (len + PAGE_SIZE - 1) / PAGE_SIZE)) {
        f->backing = data;
        f->backing_len = len;
        i->size = len;
    }
    mutex_unlock(&f->lock);
}

/*
 * Drop up to want clean, backed pages from the LRU (second chance: referenced pages are
 * rotated once), unmapping them from every address space through the reverse map. Never
 * sleeps: inode mutexes and mm locks are only trylocked, so it can run from the fault path
 * and with arbitrary mutexes held.
 */
static int reclaim_busy;
uint64_t tmpfs_reclaim(uint64_t want) {
    if (__atomic_exchange_n(&reclaim_busy, 1, __ATOMIC_ACQUIRE)) return 0;
    uint64_t freed = 0, scanned = 0, budget = __atomic_load_n(&pagecache_lru_pages, __ATOMIC_RELAXED) * 2 + 16;
    while (freed < want && scanned++ < budget) {
        uint64_t irq = arch_irq_save();
        spin_lock_ipi(&lru_lock);
        if (list_empty(&lru)) { spin_unlock(&lru_lock); arch_irq_restore(irq); break; }
        struct page *pg = list_first(&lru, struct page, node);
        list_del(&pg->node);
        list_add_tail(&lru, &pg->node);        /* rotate */
        if (page_uflag_test(pg, PGU_REFERENCED)) {
            page_uflag_clear(pg, PGU_REFERENCED);
            spin_unlock(&lru_lock); arch_irq_restore(irq);
            continue;
        }
        struct inode *ino = pg->mapping;
        struct tfile *f = ino->priv;
        /* while pg is on the LRU its inode cannot be evicted without this mutex */
        bool locked = mutex_trylock(&f->lock);
        spin_unlock(&lru_lock);
        arch_irq_restore(irq);
        if (!locked) continue;
        __atomic_fetch_add(&vm_stats.reclaim_scanned, 1, __ATOMIC_RELAXED);
        uint64_t fl = pg_lock(f);
        uint64_t idx = pg->index;
        paddr_t pa = page_to_phys(pg);
        bool ok = idx < f->npages && f->pages[idx] == pa && !page_uflag_test(pg, PGU_DIRTY) &&
                  page_uflag_test(pg, PGU_LRU) && rmap_unmap_file_page(pg) && page_ref_read(pg) == 1;
        if (ok) {
            f->pages[idx] = 0;
            cache_drop(pa);                       /* frees it */
            freed++;
            __atomic_fetch_add(&pagecache_reclaimed, 1, __ATOMIC_RELAXED);
        }
        pg_unlock(f, fl);
        mutex_unlock(&f->lock);
    }
    __atomic_store_n(&reclaim_busy, 0, __ATOMIC_RELEASE);
    return freed;
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
