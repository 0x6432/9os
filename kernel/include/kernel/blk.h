#pragma once
/*
 * Block layer (M29, kernel/block/). A struct blkdev is a whole disk (registered by a driver)
 * or a partition of one (GPT/MBR, scanned at registration). I/O is described by a struct bio:
 * a contiguous run of 512-byte sectors and up to BIO_MAX_VECS physical segments; completion
 * runs bio->end_io, possibly in interrupt context. Each disk has a request queue sorted by
 * sector (one-way elevator, adjacent requests dispatched back to back) that the driver drains
 * up to its queue depth.
 *
 * Every blkdev has an inode whose page cache is the buffer cache: /dev/vdX reads and writes
 * and filesystem metadata (bread) share it. Pages of a block device cache record which of
 * their blocks are dirty in page->private, so writeback never overwrites a block that a file
 * mapping rewrote in the meantime.
 */
#include <kernel/types.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/sched.h>

#define SECTOR_SIZE 512
#define BIO_MAX_VECS 32
enum { BIO_READ, BIO_WRITE, BIO_FLUSH };

struct blkdev;
struct bio_vec { paddr_t pa; uint32_t len; };
struct bio {
    struct blkdev *dev;
    uint64_t sector;             /* relative to dev (partitions are remapped on submit) */
    int op;
    int nvec;
    uint32_t size;               /* bytes */
    struct bio_vec vec[BIO_MAX_VECS];
    void (*end_io)(struct bio *b);
    void *priv;
    int status;                  /* 0 or -EIO */
    volatile int done;
    struct list_node node;       /* request queue */
    uint64_t start_ns;
};

struct blk_driver_ops {
    /* start bio (absolute sectors); false if the device is full right now (requeued) */
    bool (*submit)(struct blkdev *d, struct bio *b);
};

struct blkdev {
    char name[16];
    uint64_t nsectors;
    uint32_t sector_size;        /* logical block size reported by the device */
    uint64_t rdev;
    bool readonly, has_flush;
    unsigned max_vecs;           /* segments per bio the driver accepts (<= BIO_MAX_VECS) */
    struct blkdev *whole;        /* partitions: the disk; disks: itself */
    uint64_t start;              /* partitions: first sector on the disk */
    int partno;
    const struct blk_driver_ops *ops;
    void *driver;
    /* request queue (whole disks) */
    spinlock_t qlock;
    struct list_node queue;
    unsigned inflight, depth;
    uint64_t last_sector;        /* elevator head */
    /* statistics (/proc/diskstats) */
    uint64_t reads, writes, sectors_read, sectors_written, flushes, io_ns, merges_ahead;
    struct inode *inode;         /* buffer cache */
    struct super_block *mounted; /* filesystem using it exclusively */
};

void blk_init(void);
/* register a disk: scans partitions, creates /dev nodes; returns the disk */
struct blkdev *blk_register_disk(const char *name, uint64_t nsectors, uint32_t sector_size,
                                 const struct blk_driver_ops *ops, void *driver, unsigned depth);
/* after the driver finished setting up the disk: /dev nodes, GPT/MBR partitions */
void blk_scan_partitions(struct blkdev *disk);
struct blkdev *blk_get(uint64_t rdev);
struct file_ops;
const struct file_ops *blkdev_fops_get(uint64_t rdev);   /* S_IFBLK special files */
uint64_t blkdev_size(uint64_t rdev);
struct blkdev *blk_find(const char *name);   /* "vda1" */
int blk_count(void);
struct blkdev *blk_get_index(int i);

struct bio *bio_alloc(struct blkdev *d, int op, uint64_t sector);
void bio_free(struct bio *b);
bool bio_add(struct bio *b, paddr_t pa, uint32_t len);
void submit_bio(struct bio *b);
int submit_bio_wait(struct bio *b);          /* sleeps; returns b->status */
void bio_complete(struct bio *b, int status); /* driver: request finished (any context) */
void blk_kick(struct blkdev *disk);           /* driver: room in the device, dispatch more */
/* synchronous helpers on kernel memory (contiguous physical pages via the direct map) */
int blk_rw(struct blkdev *d, uint64_t sector, void *buf, size_t bytes, bool write);
int blk_flush(struct blkdev *d);

/* buffer cache: block blk of size bsize (power of two, 512..4096) of device d */
struct buf {
    struct page *pg;
    uint8_t *data;
    uint64_t blk;
    uint32_t size;
    struct blkdev *dev;
};
int bread(struct blkdev *d, uint64_t blk, uint32_t bsize, struct buf *out);
/* like bread but no read: the caller overwrites the whole block */
int bget_new(struct blkdev *d, uint64_t blk, uint32_t bsize, struct buf *out);
void bdirty(struct buf *b);
void brelse(struct buf *b);
/* the block was freed: forget pending writes of it in the device cache */
void bforget(struct blkdev *d, uint64_t blk, uint32_t bsize);
int blk_sync(struct blkdev *d);              /* write back the device cache + flush */
int blk_proc_partitions(char *buf, size_t max);
int blk_proc_diskstats(char *buf, size_t max);
