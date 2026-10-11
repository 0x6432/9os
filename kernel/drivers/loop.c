/*
 * M35+: loop block devices (Linux ABI): /dev/loop-control (misc 10:237) LOOP_CTL_GET_FREE /
 * ADD / REMOVE create and remove /dev/loopN (block 7:N) at run time - add/remove uevents, the
 * node and /sys/devices/virtual/block/loopN come and go, so block devices are hot-pluggable.
 * LOOP_SET_FD / LOOP_CONFIGURE bind a regular file (offset, sizelimit, read-only, autoclear),
 * LOOP_CLR_FD detaches (deferred to the last close when the device is still open, as Linux),
 * LOOP_GET/SET_STATUS64, LOOP_SET_CAPACITY; capacity changes send "change" uevents. Requests
 * are served by a kernel thread with page-cache reads/writes of the backing file.
 */
#include <kernel/blk.h>
#include <kernel/vfs.h>
#include <kernel/device.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/cpu.h>
#include <kernel/cred.h>
#include <kernel/syscall.h>
#include <kernel/uaccess.h>
#include <kernel/mm.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>

#define LOOP_MAJOR 7
#define MAX_LOOP 16
#define LO_FLAGS_READ_ONLY 1
#define LO_FLAGS_AUTOCLEAR 4
#define LO_FLAGS_PARTSCAN 8
#define LO_FLAGS_DIRECT_IO 16

struct loop_info64 {
    uint64_t lo_device, lo_inode, lo_rdevice, lo_offset, lo_sizelimit;
    uint32_t lo_number, lo_encrypt_type, lo_encrypt_key_size, lo_flags;
    uint8_t lo_file_name[64], lo_crypt_name[64], lo_encrypt_key[32];
    uint64_t lo_init[2];
};
_Static_assert(sizeof(struct loop_info64) == 232, "loop_info64");
struct loop_config { uint32_t fd, block_size; struct loop_info64 info; uint64_t reserved[8]; };
_Static_assert(sizeof(struct loop_config) == 304, "loop_config");

struct loop {
    int idx;
    struct blkdev *disk;
    struct file *backing;
    uint64_t offset, sizelimit;
    uint32_t flags;
    char name[64];
};
static struct loop *loops[MAX_LOOP];
static spinlock_t qlock = SPINLOCK_INIT;
static struct list_node queue = LIST_INIT(queue);
static struct wait_queue qwq = WAIT_QUEUE_INIT(qwq);
static bool worker_started;

static bool loop_submit(struct blkdev *d, struct bio *b) {
    uint64_t f = spin_lock_irqsave(&qlock);
    list_add_tail(&queue, &b->node);
    spin_unlock_irqrestore(&qlock, f);
    wake_up(&qwq);
    return true;
}

static int do_bio(struct loop *lo, struct bio *b) {
    struct file *f = lo->backing;
    if (!f) return -EIO;
    if (b->op == BIO_FLUSH) return 0;
    uint64_t pos = b->sector * SECTOR_SIZE + lo->offset;
    bool bkl = !f->fops->nobkl;
    if (bkl) bkl_enter();
    int err = 0;
    for (int i = 0; i < b->nvec && !err; i++) {
        uint8_t *va = PHYS_TO_VIRT(b->vec[i].pa);
        uint32_t len = b->vec[i].len;
        if (b->op == BIO_READ) {
            ssize_t r = vfs_pread(f, va, len, (off_t)pos);
            if (r < 0) err = -EIO;
            else if ((uint32_t)r < len) memset(va + r, 0, len - (uint32_t)r);     /* past the end: zeros */
        } else {
            off_t off = (off_t)pos;
            size_t done = 0;
            while (done < len) {
                ssize_t r = f->fops->write ? f->fops->write(f, va + done, len - done, &off) : -EINVAL;
                if (r <= 0) { err = -EIO; break; }
                done += (size_t)r;
            }
        }
        pos += len;
    }
    if (bkl) bkl_exit();
    return err;
}

static void worker(void *arg) {
    for (;;) {
        wait_until(&qwq, !list_empty(&queue));
        uint64_t f = spin_lock_irqsave(&qlock);
        if (list_empty(&queue)) { spin_unlock_irqrestore(&qlock, f); continue; }
        struct bio *b = list_entry(queue.next, struct bio, node);
        list_del(&b->node);
        spin_unlock_irqrestore(&qlock, f);
        struct loop *lo = b->dev->whole->driver;
        bio_complete(b, do_bio(lo, b));
    }
}

static uint64_t backing_sectors(struct loop *lo) {
    uint64_t sz = lo->backing->inode->size;
    sz = sz > lo->offset ? sz - lo->offset : 0;
    if (lo->sizelimit && lo->sizelimit < sz) sz = lo->sizelimit;
    return sz / SECTOR_SIZE;
}

static void detach(struct loop *lo) {
    struct file *f = lo->backing;
    if (!f) return;
    blk_set_capacity(lo->disk, 0);         /* flushes dirty cache pages to the file first */
    lo->backing = nullptr;
    lo->flags = 0; lo->offset = lo->sizelimit = 0; lo->name[0] = 0;
    lo->disk->readonly = false;
    vfs_close(f);
}

static void loop_last_close(struct blkdev *d) {
    struct loop *lo = d->driver;
    if (lo->backing && (lo->flags & LO_FLAGS_AUTOCLEAR)) detach(lo);
}

static const struct blk_driver_ops loop_ops = { .submit = loop_submit, .last_close = loop_last_close };

/* sysfs: the usual disk attributes plus loop/ */
static struct loop *lo_of(struct device *k) { return ((struct blkdev *)k->priv)->driver; }
static int a_size(struct device *k, char *b, size_t n) { return dev_printf(b, n, "%lu\n", ((struct blkdev *)k->priv)->nsectors); }
static int a_ro(struct device *k, char *b, size_t n) { return dev_printf(b, n, "%d\n", ((struct blkdev *)k->priv)->readonly); }
static int a_zero(struct device *k, char *b, size_t n) { return dev_printf(b, n, "0\n"); }
static int a_lbs(struct device *k, char *b, size_t n) { return dev_printf(b, n, "512\n"); }
static int a_backing(struct device *k, char *b, size_t n) { struct loop *lo = lo_of(k); return lo->backing ? dev_printf(b, n, "%s\n", lo->name) : 0; }
static int a_offset(struct device *k, char *b, size_t n) { return dev_printf(b, n, "%lu\n", lo_of(k)->offset); }
static int a_sizelimit(struct device *k, char *b, size_t n) { return dev_printf(b, n, "%lu\n", lo_of(k)->sizelimit); }
static int a_autoclear(struct device *k, char *b, size_t n) { return dev_printf(b, n, "%d\n", !!(lo_of(k)->flags & LO_FLAGS_AUTOCLEAR)); }
static int a_partscan(struct device *k, char *b, size_t n) { return dev_printf(b, n, "%d\n", !!(lo_of(k)->flags & LO_FLAGS_PARTSCAN)); }
static const struct dev_attr loop_attrs[] = {
    { "size", 0444, a_size, nullptr }, { "ro", 0444, a_ro, nullptr }, { "removable", 0444, a_zero, nullptr },
    { "queue/logical_block_size", 0444, a_lbs, nullptr }, { "queue/rotational", 0444, a_zero, nullptr },
    { "loop/backing_file", 0444, a_backing, nullptr }, { "loop/offset", 0444, a_offset, nullptr },
    { "loop/sizelimit", 0444, a_sizelimit, nullptr }, { "loop/autoclear", 0444, a_autoclear, nullptr },
    { "loop/partscan", 0444, a_partscan, nullptr }, { "loop/dio", 0444, a_zero, nullptr },
    { nullptr, 0, nullptr, nullptr },
};

static spinlock_t ctl_lock = SPINLOCK_INIT;
static int loop_add(int idx) {
    if (idx >= MAX_LOOP) return -EINVAL;
    uint64_t fl = spin_lock_irqsave(&ctl_lock);
    if (idx < 0) for (int i = 0; i < MAX_LOOP && idx < 0; i++) if (!loops[i]) idx = i;
    if (idx < 0) { spin_unlock_irqrestore(&ctl_lock, fl); return -ENOSPC; }
    if (loops[idx]) { spin_unlock_irqrestore(&ctl_lock, fl); return -EEXIST; }
    loops[idx] = (struct loop *)1;                     /* reserved */
    spin_unlock_irqrestore(&ctl_lock, fl);
    struct loop *lo = kzalloc(sizeof *lo);
    char nm[16];
    snprintf(nm, sizeof nm, "loop%d", idx);
    if (lo) { lo->idx = idx; lo->disk = blk_register_dynamic(nm, MKDEV(LOOP_MAJOR, idx), &loop_ops, lo, loop_attrs, nullptr); }
    if (!lo || !lo->disk) { kfree(lo); loops[idx] = nullptr; return -ENOMEM; }
    loops[idx] = lo;
    if (!worker_started) { worker_started = true; thread_create("kloopd", worker, nullptr); }
    return idx;
}
static bool live(struct loop *lo) { return lo && lo != (struct loop *)1; }

static int loop_remove(int idx) {
    if (idx < 0 || idx >= MAX_LOOP || !live(loops[idx])) return -ENODEV;
    struct loop *lo = loops[idx];
    if (lo->backing) return -EBUSY;
    int r = blk_unregister(lo->disk);
    if (r) return r;
    loops[idx] = nullptr;          /* lo stays allocated: the stale blkdev still points at it */
    return 0;
}

static int set_fd(struct loop *lo, int fd, const struct loop_info64 *info) {
    if (lo->backing) return -EBUSY;
    struct file *f = fd_get_ref(fd);
    if (!f) return -EBADF;
    if (!S_ISREG(f->inode->mode) && !S_ISBLK(f->inode->mode)) { vfs_close(f); return -EINVAL; }
    lo->backing = f;
    lo->flags = info ? info->lo_flags & (LO_FLAGS_READ_ONLY | LO_FLAGS_AUTOCLEAR | LO_FLAGS_PARTSCAN) : 0;
    if ((f->flags & O_ACCMODE) == O_RDONLY) lo->flags |= LO_FLAGS_READ_ONLY;
    lo->offset = info ? info->lo_offset : 0;
    lo->sizelimit = info ? info->lo_sizelimit : 0;
    snprintf(lo->name, sizeof lo->name, "%s", f->path ? f->path : "");
    lo->disk->readonly = lo->flags & LO_FLAGS_READ_ONLY;
    blk_set_capacity(lo->disk, backing_sectors(lo));
    return 0;
}

static int get_status(struct loop *lo, uint64_t arg) {
    if (!lo->backing) return -ENXIO;
    struct loop_info64 i = { 0 };
    i.lo_device = 0; i.lo_inode = lo->backing->inode->ino; i.lo_rdevice = lo->backing->inode->rdev;
    i.lo_offset = lo->offset; i.lo_sizelimit = lo->sizelimit; i.lo_number = (uint32_t)lo->idx; i.lo_flags = lo->flags;
    memcpy(i.lo_file_name, lo->name, MIN(sizeof i.lo_file_name - 1, strlen(lo->name)));
    return copy_to_user((void *)arg, &i, sizeof i) ? -EFAULT : 0;
}

static int loop_ioctl(struct blkdev *d, struct file *file, uint64_t cmd, uint64_t arg) {
    struct loop *lo = d->driver;
    switch ((uint32_t)cmd) {
    case 0x4C00: return set_fd(lo, (int)arg, nullptr);                      /* LOOP_SET_FD */
    case 0x4C0A: {                                                           /* LOOP_CONFIGURE */
        struct loop_config c;
        if (copy_from_user(&c, (void *)arg, sizeof c)) return -EFAULT;
        if (c.block_size && c.block_size != 512) return -EINVAL;
        return set_fd(lo, (int)c.fd, &c.info);
    }
    case 0x4C01:                                                             /* LOOP_CLR_FD */
        if (!lo->backing) return -ENXIO;
        if (d->opens > 1) { lo->flags |= LO_FLAGS_AUTOCLEAR; return 0; }    /* detach at the last close */
        detach(lo);
        return 0;
    case 0x4C05: return get_status(lo, arg);                                 /* LOOP_GET_STATUS64 */
    case 0x4C04: {                                                           /* LOOP_SET_STATUS64 */
        if (!lo->backing) return -ENXIO;
        struct loop_info64 i;
        if (copy_from_user(&i, (void *)arg, sizeof i)) return -EFAULT;
        lo->flags = (lo->flags & LO_FLAGS_READ_ONLY) | (i.lo_flags & (LO_FLAGS_AUTOCLEAR | LO_FLAGS_PARTSCAN));
        if (i.lo_offset != lo->offset || i.lo_sizelimit != lo->sizelimit) {
            lo->offset = i.lo_offset; lo->sizelimit = i.lo_sizelimit;
            blk_set_capacity(d, backing_sectors(lo));
        }
        return 0;
    }
    case 0x4C07:                                                             /* LOOP_SET_CAPACITY */
        if (!lo->backing) return -ENXIO;
        blk_set_capacity(d, backing_sectors(lo));
        return 0;
    case 0x4C08: return arg ? -EINVAL : 0;                                   /* LOOP_SET_DIRECT_IO: buffered only */
    case 0x4C09: return arg == 512 ? 0 : -EINVAL;                            /* LOOP_SET_BLOCK_SIZE */
    }
    return -ENOTTY;
}

/* /dev/loop-control */
static int ctl_ioctl(struct file *f, uint64_t cmd, uint64_t arg) {
    if (!capable(CAP_SYS_ADMIN)) return -EPERM;
    switch ((uint32_t)cmd) {
    case 0x4C80: return loop_add((int)arg);                                  /* LOOP_CTL_ADD */
    case 0x4C81: return loop_remove((int)arg);                               /* LOOP_CTL_REMOVE */
    case 0x4C82:                                                             /* LOOP_CTL_GET_FREE */
        for (int i = 0; i < MAX_LOOP; i++) if (live(loops[i]) && !loops[i]->backing && !loops[i]->disk->opens) return i;
        return loop_add(-1);
    }
    return -ENOTTY;
}
static const struct file_ops ctl_ops = { .nobkl = true, .ioctl = ctl_ioctl };

extern int (*blk_loop_ioctl)(struct blkdev *d, struct file *f, uint64_t cmd, uint64_t arg);
void loop_init(void) {
    blk_loop_ioctl = loop_ioctl;
    chrdev_register(10, 237, &ctl_ops);
}
