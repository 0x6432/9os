/*
 * Block layer (M29): disk/partition registry, bios, a per-disk sorted request queue, the
 * block-device page cache ("buffer cache"), GPT/MBR partition scanning and /dev/vdX block
 * special files. See kernel/blk.h.
 */
#include <kernel/blk.h>
#include <kernel/vfs.h>
#include <kernel/pagecache.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/boot.h>
#include <kernel/pmm.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>

#define BLK_MAJOR 254
#define MAX_BLKDEV 64

static struct blkdev *devs[MAX_BLKDEV];
static int ndevs, ndisks;
static const struct lock_class q_class = { "blk_queue", LR_BLKQ, false };
static struct wait_queue bio_wq = WAIT_QUEUE_INIT(bio_wq);

int blk_count(void) { return ndevs; }
struct blkdev *blk_get_index(int i) { return i >= 0 && i < ndevs ? devs[i] : nullptr; }
struct blkdev *blk_get(uint64_t rdev) {
    for (int i = 0; i < ndevs; i++) if (devs[i]->rdev == rdev) return devs[i];
    return nullptr;
}
struct blkdev *blk_find(const char *name) {
    if (!strncmp(name, "/dev/", 5)) name += 5;
    for (int i = 0; i < ndevs; i++) if (!strcmp(devs[i]->name, name)) return devs[i];
    return nullptr;
}

/* ------------------------------------------------------------------ bios and the queue */
struct bio *bio_alloc(struct blkdev *d, int op, uint64_t sector) {
    struct bio *b = kzalloc(sizeof *b);
    if (!b) return nullptr;
    b->dev = d; b->op = op; b->sector = sector;
    list_init(&b->node);
    return b;
}
void bio_free(struct bio *b) { kfree(b); }

bool bio_add(struct bio *b, paddr_t pa, uint32_t len) {
    if (b->nvec && b->vec[b->nvec - 1].pa + b->vec[b->nvec - 1].len == pa) { b->vec[b->nvec - 1].len += len; b->size += len; return true; }
    unsigned max = b->dev->whole->max_vecs ? b->dev->whole->max_vecs : BIO_MAX_VECS;
    if ((unsigned)b->nvec >= max) return false;
    b->vec[b->nvec++] = (struct bio_vec){ pa, len };
    b->size += len;
    return true;
}

/* C-LOOK among the requests before the first flush: the first one at or after the head,
 * else wrap to the lowest sector. A flush starts once everything before it completed. */
static struct bio *pick(struct blkdev *d) {
    struct bio *lowest = nullptr;
    list_for_each(it, &d->queue) {
        struct bio *b = list_entry(it, struct bio, node);
        if (b->op == BIO_FLUSH) {
            if (!lowest) return d->inflight ? nullptr : b;
            break;
        }
        if (!lowest) lowest = b;
        if (b->sector >= d->last_sector) return b;
    }
    return lowest;
}

static void dispatch(struct blkdev *d) {
    uint64_t f = arch_irq_save();
    spin_lock_ipi(&d->qlock);
    while (d->inflight < d->depth && !list_empty(&d->queue)) {
        struct bio *b = pick(d);
        if (!b) break;
        list_del(&b->node);
        d->inflight++;
        if (!d->ops->submit(d, b)) {          /* device full: retry on the next completion */
            d->inflight--;
            list_add(&d->queue, &b->node);
            break;
        }
        if (b->op != BIO_FLUSH) {
            if (b->sector == d->last_sector) d->merges_ahead++;   /* sequential with the previous one */
            d->last_sector = b->sector + b->size / SECTOR_SIZE;
        }
    }
    spin_unlock(&d->qlock);
    arch_irq_restore(f);
}

void blk_kick(struct blkdev *disk) { dispatch(disk); }

void submit_bio(struct bio *b) {
    struct blkdev *p = b->dev, *d = p->whole;
    uint64_t ns = b->op == BIO_FLUSH ? 0 : b->size / SECTOR_SIZE;
    if ((b->op == BIO_WRITE && p->readonly) || b->sector + ns > p->nsectors || (b->size % SECTOR_SIZE)) {
        bio_complete(b, -EIO);
        return;
    }
    if (b->op == BIO_FLUSH && !d->has_flush) { bio_complete(b, 0); return; }
    b->sector += p->start;
    b->start_ns = time_ns();
    b->done = 0;
    uint64_t f = arch_irq_save();
    spin_lock_ipi(&d->qlock);
    /* sorted insert (flushes keep their place at the tail: everything queued before them goes first) */
    struct list_node *at = &d->queue;
    if (b->op != BIO_FLUSH) {
        list_for_each(it, &d->queue) {
            struct bio *o = list_entry(it, struct bio, node);
            if (o->op == BIO_FLUSH) { at = &d->queue; }
            else if (o->sector > b->sector) { at = it; break; }
        }
    }
    list_add_tail(at, &b->node);         /* before `at` */
    spin_unlock(&d->qlock);
    arch_irq_restore(f);
    dispatch(d);
}

void bio_complete(struct bio *b, int status) {
    struct blkdev *p = b->dev, *d = p->whole;
    bool queued = b->start_ns != 0;
    if (queued) {
        uint64_t f = arch_irq_save();
        spin_lock_ipi(&d->qlock);
        d->inflight--;
        uint64_t ns = time_ns() - b->start_ns, sec = b->size / SECTOR_SIZE;
        for (struct blkdev *x = p;; x = d) {
            if (b->op == BIO_READ) { x->reads++; x->sectors_read += sec; }
            else if (b->op == BIO_WRITE) { x->writes++; x->sectors_written += sec; }
            else x->flushes++;
            x->io_ns += ns;
            if (x == d) break;
        }
        spin_unlock(&d->qlock);
        arch_irq_restore(f);
    }
    b->status = status;
    if (status) pr_warn("blk: %s: %s error at sector %lu\n", p->name, b->op == BIO_READ ? "read" : "write", b->sector);
    if (b->end_io) b->end_io(b);
    if (queued) dispatch(d);
}

static void wait_end_io(struct bio *b) {
    __atomic_store_n(&b->done, 1, __ATOMIC_RELEASE);
    wake_up(&bio_wq);
}

int submit_bio_wait(struct bio *b) {
    b->end_io = wait_end_io;
    submit_bio(b);
    while (!__atomic_load_n(&b->done, __ATOMIC_ACQUIRE)) {
        uint64_t f = sched_wait_lock();
        if (__atomic_load_n(&b->done, __ATOMIC_ACQUIRE)) { sched_wait_unlock(f); break; }
        wait_event_uninterruptible_locked(&bio_wq, f);
    }
    return b->status;
}

int blk_rw(struct blkdev *d, uint64_t sector, void *buf, size_t bytes, bool write) {
    uint8_t *p = buf;
    while (bytes) {
        struct bio *b = bio_alloc(d, write ? BIO_WRITE : BIO_READ, sector);
        if (!b) return -ENOMEM;
        while (bytes) {
            size_t chunk = MIN(bytes, PAGE_SIZE - ((uintptr_t)p % PAGE_SIZE));
            if (!bio_add(b, VIRT_TO_PHYS(p), (uint32_t)chunk)) break;
            p += chunk; bytes -= chunk;
        }
        sector += b->size / SECTOR_SIZE;
        int r = submit_bio_wait(b);
        bio_free(b);
        if (r) return r;
    }
    return 0;
}

int blk_flush(struct blkdev *d) {
    struct bio *b = bio_alloc(d, BIO_FLUSH, 0);
    if (!b) return -ENOMEM;
    int r = submit_bio_wait(b);
    bio_free(b);
    return r;
}

/* ------------------------------------------------------------------ the buffer cache */
#define SECT_PER_PAGE (PAGE_SIZE / SECTOR_SIZE)

static void bdev_read_end(struct bio *b) {
    filemap_read_done(b->priv, b->status);
    bio_free(b);
}

static int bdev_readpage(struct address_space *m, struct page *pg) {
    struct blkdev *d = m->host->priv;
    uint64_t sector = (uint64_t)pg->index * SECT_PER_PAGE;
    uint8_t *va = PHYS_TO_VIRT(page_to_phys(pg));
    unsigned n = sector >= d->nsectors ? 0 : (unsigned)MIN((uint64_t)SECT_PER_PAGE, d->nsectors - sector);
    if (n < SECT_PER_PAGE) memset(va + n * SECTOR_SIZE, 0, (SECT_PER_PAGE - n) * SECTOR_SIZE);
    if (!n) { filemap_read_done(pg, 0); return 0; }
    struct bio *b = bio_alloc(d, BIO_READ, sector);
    if (!b) return -ENOMEM;
    bio_add(b, page_to_phys(pg), n * SECTOR_SIZE);
    b->end_io = bdev_read_end;
    b->priv = pg;
    submit_bio(b);
    return 0;
}

/* write the runs of dirty sectors recorded in pg->private */
static int bdev_writepage(struct address_space *m, struct page *pg) {
    struct blkdev *d = m->host->priv;
    uint32_t mask = __atomic_exchange_n(&pg->private, 0, __ATOMIC_ACQ_REL);
    uint64_t base = (uint64_t)pg->index * SECT_PER_PAGE;
    int err = 0;
    for (unsigned s = 0; s < SECT_PER_PAGE;) {
        if (!(mask & (1u << s))) { s++; continue; }
        unsigned e = s;
        while (e < SECT_PER_PAGE && (mask & (1u << e))) e++;
        if (base + e > d->nsectors) e = (unsigned)(d->nsectors - base);
        if (e > s) {
            struct bio *b = bio_alloc(d, BIO_WRITE, base + s);
            if (!b) { err = -ENOMEM; break; }
            bio_add(b, page_to_phys(pg) + s * SECTOR_SIZE, (e - s) * SECTOR_SIZE);
            int r = submit_bio_wait(b);
            bio_free(b);
            if (r) err = r;
        }
        s = e + 1;
    }
    if (err) __atomic_fetch_or(&pg->private, mask, __ATOMIC_ACQ_REL);   /* retried later */
    return err;
}

static const struct aspace_ops bdev_aops = { .readpage = bdev_readpage, .writepage = bdev_writepage };

static uint32_t sector_mask(unsigned off, unsigned len) {
    unsigned s = off / SECTOR_SIZE, e = (off + len + SECTOR_SIZE - 1) / SECTOR_SIZE;
    return (uint32_t)(((1ull << e) - 1) & ~((1ull << s) - 1));
}

static int buf_get(struct blkdev *d, uint64_t blk, uint32_t bsize, struct buf *out, bool fresh) {
    uint64_t byte = blk * bsize;
    if (byte + bsize > d->nsectors * SECTOR_SIZE) return -EIO;
    struct page *pg;
    int r = filemap_get_page(d->inode->mapping, byte / PAGE_SIZE, fresh && bsize == PAGE_SIZE, &pg);
    if (r) return r;
    *out = (struct buf){ pg, (uint8_t *)PHYS_TO_VIRT(page_to_phys(pg)) + byte % PAGE_SIZE, blk, bsize, d };
    if (fresh) memset(out->data, 0, bsize);
    return 0;
}
int bread(struct blkdev *d, uint64_t blk, uint32_t bsize, struct buf *out) { return buf_get(d, blk, bsize, out, false); }
int bget_new(struct blkdev *d, uint64_t blk, uint32_t bsize, struct buf *out) { return buf_get(d, blk, bsize, out, true); }

void bdirty(struct buf *b) {
    __atomic_fetch_or(&b->pg->private, sector_mask((unsigned)(b->blk * b->size % PAGE_SIZE), b->size), __ATOMIC_ACQ_REL);
    pagecache_mark_dirty(b->pg);
}
void brelse(struct buf *b) { if (b->pg) page_put(b->pg); b->pg = nullptr; }

void bforget(struct blkdev *d, uint64_t blk, uint32_t bsize) {
    uint64_t byte = blk * bsize;
    struct page *pg = filemap_find(d->inode->mapping, byte / PAGE_SIZE);
    if (!pg) return;
    __atomic_fetch_and(&pg->private, ~sector_mask((unsigned)(byte % PAGE_SIZE), bsize), __ATOMIC_ACQ_REL);
    page_put(pg);
}

int blk_sync(struct blkdev *d) {
    int r = filemap_writeback(d->inode->mapping, 0, UINT64_MAX);
    int f = blk_flush(d);
    return r ? r : f;
}

/* ------------------------------------------------------------------ /dev/vdX files */
#define BLKGETSIZE   0x1260
#define BLKFLSBUF    0x1261
#define BLKSSZGET    0x1268
#define BLKGETSIZE64 0x80081272
#define BLKPBSZGET   0x127b
#define BLKRRPART    0x125f

static struct blkdev *file_blk(struct file *f) { return blk_get(f->inode->rdev); }

static ssize_t bdev_read(struct file *f, void *buf, size_t n, off_t *off) {
    struct blkdev *d = file_blk(f);
    if (!d) return -ENXIO;
    if (*off < 0) return -EINVAL;
    ssize_t r = filemap_read(d->inode->mapping, d->nsectors * SECTOR_SIZE, buf, n, *off);
    if (r > 0) *off += r;
    return r;
}

static ssize_t bdev_write(struct file *f, const void *buf, size_t n, off_t *off) {
    struct blkdev *d = file_blk(f);
    if (!d) return -ENXIO;
    if (d->readonly) return -EROFS;
    if (*off < 0) return -EINVAL;
    uint64_t size = d->nsectors * SECTOR_SIZE;
    if ((uint64_t)*off >= size) return n ? -ENOSPC : 0;
    n = MIN(n, size - *off);
    size_t done = 0;
    int err = 0;
    while (done < n) {
        uint64_t pos = *off + done;
        unsigned po = pos % PAGE_SIZE;
        size_t chunk = MIN(n - done, PAGE_SIZE - po);
        struct page *pg;
        /* whole sectors need no read; partial ones read the page first */
        bool whole = po == 0 && chunk == PAGE_SIZE;
        if ((err = filemap_get_page(d->inode->mapping, pos / PAGE_SIZE, whole, &pg))) break;
        err = pc_copy_in((uint8_t *)PHYS_TO_VIRT(page_to_phys(pg)) + po, (const uint8_t *)buf + done, chunk);
        if (!err) {
            __atomic_fetch_or(&pg->private, sector_mask(po, (unsigned)chunk), __ATOMIC_ACQ_REL);
            pagecache_mark_dirty(pg);
        }
        page_put(pg);
        if (err) { err = -EFAULT; break; }
        done += chunk;
    }
    if (done) *off += done;
    if ((f->flags & O_SYNC) == O_SYNC && done) blk_sync(d);
    return done ? (ssize_t)done : err;
}

static int bdev_ioctl(struct file *f, uint64_t cmd, uint64_t arg) {
    struct blkdev *d = file_blk(f);
    if (!d) return -ENXIO;
    switch ((uint32_t)cmd) {      /* the request is an unsigned int (sign-extended by libc) */
    case BLKGETSIZE64: { uint64_t v = d->nsectors * SECTOR_SIZE; return copy_to_user((void *)arg, &v, 8) ? -EFAULT : 0; }
    case BLKGETSIZE: { unsigned long v = d->nsectors; return copy_to_user((void *)arg, &v, sizeof v) ? -EFAULT : 0; }
    case BLKSSZGET: case BLKPBSZGET: { int v = (int)d->sector_size; return copy_to_user((void *)arg, &v, 4) ? -EFAULT : 0; }
    case BLKFLSBUF: return blk_sync(d);
    case BLKRRPART: return -EBUSY;
    }
    return -ENOTTY;
}

static unsigned bdev_poll(struct file *f) { return POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM; }
static const struct file_ops bdev_fops = { .nobkl = true, .read = bdev_read, .write = bdev_write, .ioctl = bdev_ioctl, .poll = bdev_poll };

const struct file_ops *blkdev_fops_get(uint64_t rdev) { return blk_get(rdev) ? &bdev_fops : nullptr; }
uint64_t blkdev_size(uint64_t rdev) { struct blkdev *d = blk_get(rdev); return d ? d->nsectors * SECTOR_SIZE : 0; }

/* ------------------------------------------------------------------ registration */
static struct blkdev *add_dev(const char *name, uint64_t nsect, struct blkdev *whole, uint64_t start, int partno, int disk) {
    if (ndevs >= MAX_BLKDEV) return nullptr;
    struct blkdev *d = kzalloc(sizeof *d);
    struct inode *i = inode_alloc(S_IFBLK | 0600);
    struct address_space *m = kzalloc(sizeof *m);
    if (!d || !i || !m) { kfree(d); kfree(m); return nullptr; }
    strncpy(d->name, name, sizeof d->name - 1);
    d->nsectors = nsect;
    d->whole = whole ? whole : d;
    d->start = start;
    d->partno = partno;
    d->rdev = MKDEV(BLK_MAJOR, disk * 16 + partno);
    spin_lock_init_class(&d->qlock, &q_class);
    list_init(&d->queue);
    i->rdev = d->rdev;
    i->size = nsect * SECTOR_SIZE;
    i->nlink = 1;
    i->priv = d;
    mapping_init(m, i, &bdev_aops);
    i->mapping = m;
    d->inode = i;
    devs[ndevs++] = d;
    return d;
}

static void add_part(struct blkdev *disk, int disk_no, int partno, uint64_t start, uint64_t count) {
    if (!count || start >= disk->nsectors || partno > 15) return;
    if (start + count > disk->nsectors) count = disk->nsectors - start;
    char nm[16];
    snprintf(nm, sizeof nm, "%s%d", disk->name, partno);
    struct blkdev *p = add_dev(nm, count, disk, start, partno, disk_no);
    if (!p) return;
    p->sector_size = disk->sector_size;
    p->readonly = disk->readonly;
    p->has_flush = disk->has_flush;
    p->ops = disk->ops;
    p->driver = disk->driver;
    pr_info("blk: %s: sectors %lu-%lu (%lu MiB)\n", nm, start, start + count - 1, count >> 11);
}

static bool scan_gpt(struct blkdev *d, int disk_no, uint8_t *buf) {
    if (blk_rw(d, 1, buf, SECTOR_SIZE, false) || memcmp(buf, "EFI PART", 8)) return false;
    uint64_t lba; uint32_t cnt, esz;
    memcpy(&lba, buf + 72, 8); memcpy(&cnt, buf + 80, 4); memcpy(&esz, buf + 84, 4);
    if (esz < 128 || esz > 1024 || cnt > 1024) return false;
    uint64_t bytes = (uint64_t)cnt * esz, sects = (bytes + SECTOR_SIZE - 1) / SECTOR_SIZE;
    unsigned order = 0;
    while ((PAGE_SIZE << order) < sects * SECTOR_SIZE) order++;
    paddr_t pa = pmm_alloc_pages(order);
    if (!pa) return false;
    uint8_t *e = PHYS_TO_VIRT(pa);
    if (!blk_rw(d, lba, e, sects * SECTOR_SIZE, false)) {
        static const uint8_t zero[16];
        for (uint32_t k = 0; k < cnt; k++) {
            uint8_t *ent = e + (uint64_t)k * esz;
            if (!memcmp(ent, zero, 16)) continue;          /* unused entry (type GUID zero) */
            uint64_t first, last;
            memcpy(&first, ent + 32, 8); memcpy(&last, ent + 40, 8);
            if (last >= first) add_part(d, disk_no, (int)k + 1, first, last - first + 1);
        }
    }
    pmm_free_pages(pa, order);
    return true;
}

static void scan_mbr(struct blkdev *d, int disk_no, uint8_t *buf) {
    if (blk_rw(d, 0, buf, SECTOR_SIZE, false) || buf[510] != 0x55 || buf[511] != 0xaa) return;
    for (int k = 0; k < 4; k++) {
        uint8_t *ent = buf + 446 + 16 * k;
        uint32_t start, count;
        memcpy(&start, ent + 8, 4); memcpy(&count, ent + 12, 4);
        if (ent[4] == 0xee) return;                 /* protective MBR of a damaged GPT */
        if (ent[4] == 0 || ent[4] == 0x05 || ent[4] == 0x0f || ent[4] == 0x85) continue;   /* extended: unsupported */
        add_part(d, disk_no, k + 1, start, count);
    }
}

static void mknode(struct blkdev *d) {
    char path[32];
    snprintf(path, sizeof path, "/dev/%s", d->name);
    vfs_mknod_at(nullptr, path, S_IFBLK | 0660, d->rdev);
}

struct blkdev *blk_register_disk(const char *name, uint64_t nsectors, uint32_t sector_size,
                                 const struct blk_driver_ops *ops, void *driver, unsigned depth) {
    int disk_no = ndisks++;
    struct blkdev *d = add_dev(name, nsectors, nullptr, 0, 0, disk_no);
    if (!d) return nullptr;
    d->sector_size = sector_size ? sector_size : SECTOR_SIZE;
    d->ops = ops;
    d->driver = driver;
    d->depth = depth ? depth : 1;
    return d;
}

/* after the driver set readonly/has_flush and can take requests */
void blk_scan_partitions(struct blkdev *d) {
    int first = ndevs;
    int disk_no = MINOR(d->rdev) / 16;
    mknode(d);
    paddr_t pa = pmm_alloc_pages(0);
    if (!pa) return;
    uint8_t *buf = PHYS_TO_VIRT(pa);
    if (!scan_gpt(d, disk_no, buf)) scan_mbr(d, disk_no, buf);
    pmm_free_pages(pa, 0);
    for (int i = first; i < ndevs; i++) mknode(devs[i]);
}

int blk_proc_partitions(char *buf, size_t max) {
    size_t n = snprintf(buf, max, "major minor  #blocks  name\n\n");
    for (int i = 0; i < ndevs && n < max; i++)
        n += snprintf(buf + n, max - n, "%4lu  %7lu %10lu %s\n", MAJOR(devs[i]->rdev), MINOR(devs[i]->rdev),
                      devs[i]->nsectors / 2, devs[i]->name);
    return (int)MIN(n, max);
}

int blk_proc_diskstats(char *buf, size_t max) {
    size_t n = 0;
    for (int i = 0; i < ndevs && n < max; i++) {
        struct blkdev *d = devs[i];
        uint64_t ms = d->io_ns / 1000000;
        n += snprintf(buf + n, max - n, "%4lu %7lu %s %lu 0 %lu %lu %lu 0 %lu %lu %u %lu %lu 0 0 0 0 %lu %lu\n",
                      MAJOR(d->rdev), MINOR(d->rdev), d->name, d->reads, d->sectors_read, ms,
                      d->writes, d->sectors_written, ms, d == d->whole ? d->inflight : 0, ms, ms, d->flushes, ms);
    }
    return (int)MIN(n, max);
}

void blk_init(void) {}
