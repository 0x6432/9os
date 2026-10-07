/*
 * ext2 (M30): revision 0/1 filesystems with 1-4 KiB blocks, read-write. Supported features:
 * filetype (incompat), sparse_super and large_file (ro_compat); ext_attr/resize_inode/dir_index
 * (compat) are tolerated: extended attributes are left alone, the reserved GDT blocks stay
 * allocated, and directories we modify lose their htree flag (the linear format stays valid).
 * Anything else incompatible refuses to mount; unknown ro_compat features mount read-only.
 *
 * Data paths:
 *  - regular files: the inode's page cache (mm/filemap.c). readpage maps blocks (holes read as
 *    zeroes) and reads contiguous runs asynchronously; blocks are allocated by prepare_write
 *    (write(2), with the page already uptodate) or by writepage (shared mmap writes into holes);
 *  - metadata (superblock, group descriptors, bitmaps, inode tables, indirect blocks) and the
 *    contents of directories and slow symlinks: the block device's cache (bread/bdirty), i.e.
 *    the same unified page cache, written back by the writeback thread or sync/fsync.
 * Inode metadata is written through to the inode-table block on every change.
 *
 * Locking: the VFS namespace mutex serialises directory changes (and so every inode
 * allocation); ei->lock serialises writers/truncate of a file; ei->map guards a file's block
 * map against concurrent allocation (write vs. mmap writeback) and truncation; fs->alloc guards
 * bitmaps, group descriptors and the superblock counters. Order: ns -> ei->lock -> ei->map ->
 * fs->icache -> fs->alloc.
 *
 * Not implemented: journaling (a crash may need e2fsck), orphan lists (an unlinked file still
 * open at a crash leaks its inode until fsck), extended attributes/ACLs, htree lookups.
 */
#include <kernel/vfs.h>
#include <kernel/blk.h>
#include <kernel/pagecache.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/mutex.h>
#include <kernel/process.h>
#include <kernel/boot.h>
#include <kernel/time.h>

#define EXT2_MAGIC 0xef53
#define INCOMPAT_FILETYPE 0x2
#define RO_SPARSE 0x1
#define RO_LARGE  0x2
#define RO_BTREE  0x4
#define EXT2_INDEX_FL 0x1000
#define ROOT_INO 2
#define STATE_VALID 1

struct e2_sb {
    uint32_t inodes_count, blocks_count, r_blocks_count, free_blocks, free_inodes, first_data_block,
             log_block_size, log_frag_size, blocks_per_group, frags_per_group, inodes_per_group, mtime, wtime;
    uint16_t mnt_count, max_mnt_count, magic, state, errors, minor_rev;
    uint32_t lastcheck, checkinterval, creator_os, rev_level;
    uint16_t def_resuid, def_resgid;
    uint32_t first_ino;
    uint16_t inode_size, block_group_nr;
    uint32_t feature_compat, feature_incompat, feature_ro_compat;
    uint8_t uuid[16];
    char volume_name[16];
};
struct e2_gd { uint32_t block_bitmap, inode_bitmap, inode_table; uint16_t free_blocks, free_inodes, used_dirs, pad; uint32_t reserved[3]; };
struct e2_raw {
    uint16_t mode, uid; uint32_t size, atime, ctime, mtime, dtime;
    uint16_t gid, links; uint32_t blocks, flags, osd1, block[15], generation, file_acl, size_high, faddr;
    uint8_t frag, fsize; uint16_t pad, uid_high, gid_high; uint32_t reserved;
};
struct e2_dirent { uint32_t inode; uint16_t rec_len; uint8_t name_len, type; char name[]; };
_Static_assert(sizeof(struct e2_raw) == 128, "ext2 inode");
_Static_assert(sizeof(struct e2_gd) == 32, "ext2 group descriptor");

struct e2fs {
    struct super_block sb;
    struct blkdev *dev;
    uint32_t bsize, ppb, spb, ipg, bpg, ngroups, isize, first_ino, first_data, rev;
    uint8_t raw[1024];            /* superblock (in-memory master copy) */
    struct e2_gd *gdt;
    bool sb_dirty;
    struct mutex alloc, icache;
    uint32_t gen;
};
struct e2inode {
    struct inode v;
    struct address_space as;
    struct mutex lock, map;
    spinlock_t wlock;             /* serialises write_inode snapshots */
    uint32_t blk[15], flags, dtime, nblocks, file_acl, generation, last_alloc;
};
#define E2FS(s) ((struct e2fs *)(s))
#define EI(i) ((struct e2inode *)(i))
#define FS_OF(i) E2FS((i)->sb)
#define RAWSB(fs) ((struct e2_sb *)(fs)->raw)

static const struct lock_class lock_class_ = { "ext2_inode", LR_MUTEX_INODE, false };
static const struct lock_class map_class = { "ext2_bmap", LR_MUTEX_BMAP, false };
static const struct lock_class icache_class = { "ext2_icache", LR_MUTEX_ICACHE, false };
static const struct lock_class alloc_class = { "ext2_alloc", LR_MUTEX_FSALLOC, false };

static const struct inode_ops e2_iops;
static const struct file_ops e2_fops, e2_dir_fops, e2_lnk_fops;
static const struct aspace_ops e2_aops;
static int e2_write_inode(struct inode *i);

uint64_t ext2_balloc_count, ext2_bfree_count;

static bool rdonly(struct e2fs *fs) { return fs->sb.flags & SB_RDONLY; }

/* ------------------------------------------------------------------ superblock / groups */
static int sb_write(struct e2fs *fs) {
    struct buf b;
    uint64_t blk = fs->bsize == 1024 ? 1 : 0;
    int r = bread(fs->dev, blk, fs->bsize, &b);
    if (r) return r;
    RAWSB(fs)->wtime = (uint32_t)now_timespec().tv_sec;
    memcpy(b.data + (fs->bsize == 1024 ? 0 : 1024), fs->raw, 1024);
    bdirty(&b);
    brelse(&b);
    fs->sb_dirty = false;
    return 0;
}

static int gd_write(struct e2fs *fs, uint32_t g) {
    uint64_t off = (uint64_t)g * sizeof(struct e2_gd);
    struct buf b;
    int r = bread(fs->dev, fs->first_data + 1 + off / fs->bsize, fs->bsize, &b);
    if (r) return r;
    memcpy(b.data + off % fs->bsize, &fs->gdt[g], sizeof(struct e2_gd));
    bdirty(&b);
    brelse(&b);
    return 0;
}

/* first clear bit in [from, n) of a bitmap, or -1 */
static int find_zero(const uint8_t *bm, uint32_t from, uint32_t n) {
    for (uint32_t i = from; i < n;) {
        if (!(i & 63) && i + 64 <= n && *(const uint64_t *)(bm + i / 8) == ~0ull) { i += 64; continue; }
        if (!(bm[i / 8] & (1 << (i % 8)))) return (int)i;
        i++;
    }
    return -1;
}

static int balloc(struct e2fs *fs, uint32_t goal, uint32_t *out) {
    mutex_lock(&fs->alloc);
    struct e2_sb *s = RAWSB(fs);
    if (!s->free_blocks) { mutex_unlock(&fs->alloc); return -ENOSPC; }
    uint32_t g0 = goal >= fs->first_data && goal < s->blocks_count ? (goal - fs->first_data) / fs->bpg : 0;
    for (uint32_t k = 0; k <= fs->ngroups; k++) {
        uint32_t g = (g0 + k) % fs->ngroups;
        if (!fs->gdt[g].free_blocks) continue;
        uint32_t n = MIN(fs->bpg, s->blocks_count - fs->first_data - g * fs->bpg);
        struct buf b;
        if (bread(fs->dev, fs->gdt[g].block_bitmap, fs->bsize, &b)) continue;
        uint32_t start = k == 0 && goal >= fs->first_data ? (goal - fs->first_data) % fs->bpg : 0;
        int bit = find_zero(b.data, start, n);
        if (bit < 0 && start) bit = find_zero(b.data, 0, start);
        if (bit < 0) { brelse(&b); continue; }
        b.data[bit / 8] |= 1 << (bit % 8);
        bdirty(&b);
        brelse(&b);
        fs->gdt[g].free_blocks--;
        s->free_blocks--;
        fs->sb_dirty = true;
        gd_write(fs, g);
        mutex_unlock(&fs->alloc);
        *out = fs->first_data + g * fs->bpg + (uint32_t)bit;
        ext2_balloc_count++;
        return 0;
    }
    mutex_unlock(&fs->alloc);
    return -ENOSPC;
}

static void bfree(struct e2fs *fs, uint32_t blk) {
    if (blk < fs->first_data || blk >= RAWSB(fs)->blocks_count) { pr_warn("ext2: freeing bad block %u\n", blk); return; }
    bforget(fs->dev, blk, fs->bsize);
    mutex_lock(&fs->alloc);
    uint32_t g = (blk - fs->first_data) / fs->bpg, bit = (blk - fs->first_data) % fs->bpg;
    struct buf b;
    if (!bread(fs->dev, fs->gdt[g].block_bitmap, fs->bsize, &b)) {
        if (b.data[bit / 8] & (1 << (bit % 8))) {
            b.data[bit / 8] &= ~(1 << (bit % 8));
            bdirty(&b);
            fs->gdt[g].free_blocks++;
            RAWSB(fs)->free_blocks++;
            fs->sb_dirty = true;
            gd_write(fs, g);
            ext2_bfree_count++;
        } else pr_warn("ext2: double free of block %u\n", blk);
        brelse(&b);
    }
    mutex_unlock(&fs->alloc);
}

static int ialloc(struct e2fs *fs, uint32_t near, bool dir, uint32_t *out) {
    mutex_lock(&fs->alloc);
    struct e2_sb *s = RAWSB(fs);
    if (!s->free_inodes) { mutex_unlock(&fs->alloc); return -ENOSPC; }
    uint32_t g0 = near < fs->ngroups ? near : 0;
    if (dir) {          /* spread directories: the group with the most free inodes and blocks */
        uint32_t best = g0; uint64_t score = 0;
        for (uint32_t g = 0; g < fs->ngroups; g++) {
            uint64_t sc = (uint64_t)fs->gdt[g].free_inodes * 65536 / (fs->gdt[g].used_dirs + 1) + fs->gdt[g].free_blocks;
            if (fs->gdt[g].free_inodes && sc > score) { score = sc; best = g; }
        }
        g0 = best;
    }
    for (uint32_t k = 0; k < fs->ngroups; k++) {
        uint32_t g = (g0 + k) % fs->ngroups;
        if (!fs->gdt[g].free_inodes) continue;
        struct buf b;
        if (bread(fs->dev, fs->gdt[g].inode_bitmap, fs->bsize, &b)) continue;
        int bit = find_zero(b.data, 0, fs->ipg);
        while (bit >= 0 && g * fs->ipg + (uint32_t)bit + 1 < fs->first_ino) bit = find_zero(b.data, bit + 1, fs->ipg);
        if (bit < 0) { brelse(&b); continue; }
        b.data[bit / 8] |= 1 << (bit % 8);
        bdirty(&b);
        brelse(&b);
        fs->gdt[g].free_inodes--;
        if (dir) fs->gdt[g].used_dirs++;
        s->free_inodes--;
        fs->sb_dirty = true;
        gd_write(fs, g);
        mutex_unlock(&fs->alloc);
        *out = g * fs->ipg + (uint32_t)bit + 1;
        return 0;
    }
    mutex_unlock(&fs->alloc);
    return -ENOSPC;
}

static void ifree(struct e2fs *fs, uint32_t ino, bool dir) {
    mutex_lock(&fs->alloc);
    uint32_t g = (ino - 1) / fs->ipg, bit = (ino - 1) % fs->ipg;
    struct buf b;
    if (g < fs->ngroups && !bread(fs->dev, fs->gdt[g].inode_bitmap, fs->bsize, &b)) {
        if (b.data[bit / 8] & (1 << (bit % 8))) {
            b.data[bit / 8] &= ~(1 << (bit % 8));
            bdirty(&b);
            fs->gdt[g].free_inodes++;
            if (dir && fs->gdt[g].used_dirs) fs->gdt[g].used_dirs--;
            RAWSB(fs)->free_inodes++;
            fs->sb_dirty = true;
            gd_write(fs, g);
        }
        brelse(&b);
    }
    mutex_unlock(&fs->alloc);
}

/* ------------------------------------------------------------------ inodes */
static int inode_buf(struct e2fs *fs, uint32_t ino, struct buf *b, struct e2_raw **raw) {
    if (!ino || ino > RAWSB(fs)->inodes_count) return -EIO;
    uint32_t g = (ino - 1) / fs->ipg, idx = (ino - 1) % fs->ipg;
    uint64_t off = (uint64_t)idx * fs->isize;
    int r = bread(fs->dev, fs->gdt[g].inode_table + off / fs->bsize, fs->bsize, b);
    if (r) return r;
    *raw = (struct e2_raw *)(b->data + off % fs->bsize);
    return 0;
}

static bool has_blocks(struct inode *i) {
    if (S_ISCHR(i->mode) || S_ISBLK(i->mode) || S_ISFIFO(i->mode) || S_ISSOCK(i->mode)) return false;
    if (S_ISLNK(i->mode)) {
        struct e2inode *ei = EI(i);
        uint32_t acl = ei->file_acl ? FS_OF(i)->spb : 0;
        return ei->nblocks > acl;      /* fast symlinks keep the target in blk[] */
    }
    return true;
}

static void e2_setup(struct e2fs *fs, struct e2inode *ei) {
    struct inode *i = &ei->v;
    i->iops = &e2_iops;
    i->dev = fs->sb.dev;
    i->sb = &fs->sb;
    mutex_init(&ei->lock, &lock_class_);
    mutex_init(&ei->map, &map_class);
    spin_lock_init_class(&ei->wlock, nullptr);
    mapping_init(&ei->as, i, &e2_aops);
    if (S_ISREG(i->mode)) { i->fops = &e2_fops; i->mapping = &ei->as; }
    else if (S_ISDIR(i->mode)) i->fops = &e2_dir_fops;
    else if (S_ISLNK(i->mode)) i->fops = &e2_lnk_fops;
}

static int e2_iget(struct e2fs *fs, uint32_t ino, struct inode **out) {
    struct inode *i = icache_find(&fs->sb, ino);
    if (i) { *out = i; return 0; }
    mutex_lock(&fs->icache);
    if ((i = icache_find(&fs->sb, ino))) { mutex_unlock(&fs->icache); *out = i; return 0; }
    struct buf b;
    struct e2_raw *r;
    int e = inode_buf(fs, ino, &b, &r);
    if (e) { mutex_unlock(&fs->icache); return e; }
    if (!r->links && !r->mode) { brelse(&b); mutex_unlock(&fs->icache); return -ESTALE; }
    struct e2inode *ei = kzalloc(sizeof *ei);
    if (!ei) { brelse(&b); mutex_unlock(&fs->icache); return -ENOMEM; }
    i = &ei->v;
    inode_init(i, r->mode);
    i->ino = ino;
    i->uid = r->uid | (uint32_t)r->uid_high << 16;
    i->gid = r->gid | (uint32_t)r->gid_high << 16;
    i->nlink = r->links;
    i->size = r->size | (S_ISREG(r->mode) ? (uint64_t)r->size_high << 32 : 0);
    i->atime = (struct timespec){ r->atime, 0 };
    i->mtime = (struct timespec){ r->mtime, 0 };
    i->ctime = (struct timespec){ r->ctime, 0 };
    memcpy(ei->blk, r->block, sizeof ei->blk);
    ei->flags = r->flags; ei->dtime = r->dtime; ei->nblocks = r->blocks;
    ei->file_acl = r->file_acl; ei->generation = r->generation;
    brelse(&b);
    e2_setup(fs, ei);
    if (S_ISCHR(i->mode) || S_ISBLK(i->mode))
        i->rdev = ei->blk[0] ? ei->blk[0] : MKDEV((ei->blk[1] >> 8) & 0xfff, (ei->blk[1] & 0xff) | ((ei->blk[1] >> 12) & 0xfff00));
    icache_insert(&fs->sb, i);
    mutex_unlock(&fs->icache);
    *out = i;
    return 0;
}

static int e2_write_inode(struct inode *i) {
    struct e2fs *fs = FS_OF(i);
    struct e2inode *ei = EI(i);
    if (rdonly(fs)) return 0;
    struct buf b;
    struct e2_raw *r;
    int e = inode_buf(fs, (uint32_t)i->ino, &b, &r);
    if (e) return e;
    uint64_t f = arch_irq_save();
    spin_lock(&ei->wlock);
    r->mode = (uint16_t)i->mode;
    r->uid = (uint16_t)i->uid; r->uid_high = (uint16_t)(i->uid >> 16);
    r->gid = (uint16_t)i->gid; r->gid_high = (uint16_t)(i->gid >> 16);
    r->size = (uint32_t)i->size;
    if (S_ISREG(i->mode)) r->size_high = (uint32_t)(i->size >> 32);
    r->atime = (uint32_t)i->atime.tv_sec; r->mtime = (uint32_t)i->mtime.tv_sec; r->ctime = (uint32_t)i->ctime.tv_sec;
    r->dtime = ei->dtime;
    r->links = (uint16_t)i->nlink;
    r->blocks = ei->nblocks;
    r->flags = ei->flags;
    r->generation = ei->generation;
    memcpy(r->block, ei->blk, sizeof ei->blk);
    spin_unlock(&ei->wlock);
    arch_irq_restore(f);
    bdirty(&b);
    brelse(&b);
    if (S_ISREG(i->mode) && i->size > 0x7fffffffu && !(RAWSB(fs)->feature_ro_compat & RO_LARGE)) {
        mutex_lock(&fs->alloc);
        RAWSB(fs)->feature_ro_compat |= RO_LARGE;
        if (RAWSB(fs)->rev_level == 0) RAWSB(fs)->rev_level = 1;
        fs->sb_dirty = true;
        mutex_unlock(&fs->alloc);
    }
    return 0;
}

/* ------------------------------------------------------------------ block map */
static uint32_t goal_of(struct e2fs *fs, struct e2inode *ei) {
    if (ei->last_alloc) return ei->last_alloc + 1;
    return fs->first_data + (uint32_t)((ei->v.ino - 1) / fs->ipg) * fs->bpg;
}

static int new_meta_block(struct e2fs *fs, struct e2inode *ei, uint32_t *out) {
    int r = balloc(fs, goal_of(fs, ei), out);
    if (r) return r;
    struct buf b;
    if ((r = bget_new(fs->dev, *out, fs->bsize, &b))) { bfree(fs, *out); return r; }
    bdirty(&b);
    brelse(&b);
    ei->nblocks += fs->spb;
    ei->last_alloc = *out;
    return 0;
}

/*
 * Logical block -> physical block (0: hole). alloc (ei->map held for files): allocate missing
 * indirect blocks (zeroed) and the data block (not zeroed: the caller owns its contents);
 * *fresh tells whether the data block was new.
 */
static int bmap(struct e2inode *ei, uint64_t lblk, bool alloc, uint32_t *out, bool *fresh) {
    struct e2fs *fs = FS_OF(&ei->v);
    uint64_t ppb = fs->ppb;
    unsigned off[3], depth;
    uint32_t *root;
    if (fresh) *fresh = false;
    *out = 0;
    if (lblk < 12) {
        if (!ei->blk[lblk] && alloc) {
            uint32_t nb;
            int r = balloc(fs, goal_of(fs, ei), &nb);
            if (r) return r;
            ei->blk[lblk] = nb; ei->nblocks += fs->spb; ei->last_alloc = nb;
            if (fresh) *fresh = true;
        }
        *out = ei->blk[lblk];
        return 0;
    }
    lblk -= 12;
    if (lblk < ppb) { depth = 1; off[0] = (unsigned)lblk; root = &ei->blk[12]; }
    else if ((lblk -= ppb) < ppb * ppb) { depth = 2; off[0] = (unsigned)(lblk / ppb); off[1] = (unsigned)(lblk % ppb); root = &ei->blk[13]; }
    else if ((lblk -= ppb * ppb) < ppb * ppb * ppb) {
        depth = 3; off[0] = (unsigned)(lblk / (ppb * ppb)); off[1] = (unsigned)(lblk / ppb % ppb); off[2] = (unsigned)(lblk % ppb);
        root = &ei->blk[14];
    } else return -EFBIG;
    if (!*root) {
        if (!alloc) return 0;
        uint32_t nb;
        int r = new_meta_block(fs, ei, &nb);
        if (r) return r;
        *root = nb;
    }
    uint32_t cur = *root;
    for (unsigned lv = 0; lv < depth; lv++) {
        struct buf b;
        int r = bread(fs->dev, cur, fs->bsize, &b);
        if (r) return r;
        uint32_t *tab = (uint32_t *)b.data;
        uint32_t nx = __atomic_load_n(&tab[off[lv]], __ATOMIC_ACQUIRE);
        if (!nx) {
            if (!alloc) { brelse(&b); return 0; }
            if (lv + 1 < depth) r = new_meta_block(fs, ei, &nx);
            else {
                r = balloc(fs, goal_of(fs, ei), &nx);
                if (!r) { ei->nblocks += fs->spb; ei->last_alloc = nx; if (fresh) *fresh = true; }
            }
            if (r) { brelse(&b); return r; }
            __atomic_store_n(&tab[off[lv]], nx, __ATOMIC_RELEASE);
            bdirty(&b);
        }
        brelse(&b);
        cur = nx;
    }
    *out = cur;
    return 0;
}

/* free every block of the subtree rooted at blk (depth levels of indirection) mapping logical
 * blocks >= from; true if blk itself became empty (the caller frees it) */
static bool free_tree(struct e2inode *ei, uint32_t blk, unsigned depth, uint64_t base, uint64_t from) {
    struct e2fs *fs = FS_OF(&ei->v);
    uint64_t cspan = 1;
    for (unsigned k = 1; k < depth; k++) cspan *= fs->ppb;
    if (base + cspan * fs->ppb <= from) return false;
    struct buf b;
    if (bread(fs->dev, blk, fs->bsize, &b)) return false;
    uint32_t *tab = (uint32_t *)b.data;
    bool any = false, dirty = false;
    for (uint32_t k = 0; k < fs->ppb; k++) {
        uint32_t c = tab[k];
        if (!c) continue;
        uint64_t cb = base + k * cspan;
        if (cb + cspan <= from) { any = true; continue; }
        if (depth == 1 || free_tree(ei, c, depth - 1, cb, from)) {
            bfree(fs, c);
            ei->nblocks -= fs->spb;
            tab[k] = 0;
            dirty = true;
        } else any = true;
    }
    if (dirty) bdirty(&b);
    brelse(&b);
    return !any;
}

static void free_from(struct e2inode *ei, uint64_t from) {
    struct e2fs *fs = FS_OF(&ei->v);
    if (!has_blocks(&ei->v)) return;
    for (unsigned k = 0; k < 12; k++)
        if (k >= from && ei->blk[k]) { bfree(fs, ei->blk[k]); ei->blk[k] = 0; ei->nblocks -= fs->spb; }
    uint64_t base = 12, span = fs->ppb;
    for (unsigned lv = 1; lv <= 3; lv++) {
        uint32_t *slot = &ei->blk[11 + lv];
        if (*slot && free_tree(ei, *slot, lv, base, from)) {
            bfree(fs, *slot);
            *slot = 0;
            ei->nblocks -= fs->spb;
        }
        base += span;
        span *= fs->ppb;
    }
    ei->last_alloc = 0;
}

/* ------------------------------------------------------------------ file pages */
struct rp { int pending, err; struct page *pg; };

static void rp_put(struct rp *rp) {
    if (__atomic_sub_fetch(&rp->pending, 1, __ATOMIC_ACQ_REL)) return;
    filemap_read_done(rp->pg, rp->err);
    kfree(rp);
}
static void rp_end(struct bio *b) {
    struct rp *rp = b->priv;
    if (b->status) rp->err = -EIO;
    bio_free(b);
    rp_put(rp);
}

static int e2_readpage(struct address_space *m, struct page *pg) {
    struct e2inode *ei = EI(m->host);
    struct e2fs *fs = FS_OF(m->host);
    struct rp *rp = kzalloc(sizeof *rp);
    if (!rp) return -ENOMEM;
    rp->pending = 1; rp->pg = pg;
    uint8_t *va = PHYS_TO_VIRT(page_to_phys(pg));
    unsigned per = PAGE_SIZE / fs->bsize;
    uint64_t first = (uint64_t)pg->index * per, size = __atomic_load_n(&m->host->size, __ATOMIC_ACQUIRE);
    struct bio *b = nullptr;
    uint32_t last = 0;
    for (unsigned k = 0; k < per; k++) {
        uint32_t pb = 0;
        if ((first + k) * fs->bsize < size && bmap(ei, first + k, false, &pb, nullptr)) rp->err = -EIO;
        if (!pb) { memset(va + k * fs->bsize, 0, fs->bsize); continue; }
        if (b && pb == last + 1 && bio_add(b, page_to_phys(pg) + k * fs->bsize, fs->bsize)) { last = pb; continue; }
        if (b) { rp->pending++; submit_bio(b); }
        b = bio_alloc(fs->dev, BIO_READ, (uint64_t)pb * fs->spb);
        if (!b) { rp->err = -ENOMEM; break; }
        b->end_io = rp_end; b->priv = rp;
        bio_add(b, page_to_phys(pg) + k * fs->bsize, fs->bsize);
        last = pb;
    }
    if (b) { rp->pending++; submit_bio(b); }
    rp_put(rp);
    return 0;
}

static int e2_writepage(struct address_space *m, struct page *pg) {
    struct inode *i = m->host;
    struct e2inode *ei = EI(i);
    struct e2fs *fs = FS_OF(i);
    if (rdonly(fs)) return -EROFS;
    unsigned per = PAGE_SIZE / fs->bsize;
    uint64_t first = (uint64_t)pg->index * per, size = __atomic_load_n(&i->size, __ATOMIC_ACQUIRE);
    uint32_t pbs[PAGE_SIZE / 1024];
    unsigned n = 0;
    int r = 0;
    bool fresh = false;
    mutex_lock(&ei->map);
    for (; n < per && (first + n) * fs->bsize < size; n++) {
        bool f;
        if ((r = bmap(ei, first + n, true, &pbs[n], &f))) break;
        fresh |= f;
    }
    mutex_unlock(&ei->map);
    if (fresh) e2_write_inode(i);
    if (r) return r;
    for (unsigned k = 0; k < n;) {
        unsigned e = k + 1;
        while (e < n && pbs[e] == pbs[e - 1] + 1) e++;
        struct bio *b = bio_alloc(fs->dev, BIO_WRITE, (uint64_t)pbs[k] * fs->spb);
        if (!b) return -ENOMEM;
        bio_add(b, page_to_phys(pg) + k * fs->bsize, (e - k) * fs->bsize);
        int w = submit_bio_wait(b);
        bio_free(b);
        if (w) r = w;
        k = e;
    }
    return r;
}

static int e2_prepare_write(struct address_space *m, uint64_t idx, unsigned from, unsigned to) {
    struct inode *i = m->host;
    struct e2inode *ei = EI(i);
    struct e2fs *fs = FS_OF(i);
    if (rdonly(fs)) return -EROFS;
    uint64_t first = (idx * PAGE_SIZE + from) / fs->bsize, last = (idx * PAGE_SIZE + to - 1) / fs->bsize;
    bool fresh = false;
    int r = 0;
    mutex_lock(&ei->map);
    for (uint64_t l = first; l <= last; l++) {
        uint32_t pb; bool f;
        if ((r = bmap(ei, l, true, &pb, &f))) break;
        fresh |= f;
    }
    mutex_unlock(&ei->map);
    if (fresh) e2_write_inode(i);
    return r;
}

static const struct aspace_ops e2_aops = { .readpage = e2_readpage, .writepage = e2_writepage, .prepare_write = e2_prepare_write };

static ssize_t e2_read(struct file *fl, void *buf, size_t n, off_t *off) {
    struct inode *i = fl->inode;
    if (*off < 0) return -EINVAL;
    ssize_t r = filemap_read(&EI(i)->as, __atomic_load_n(&i->size, __ATOMIC_ACQUIRE), buf, n, *off);
    if (r > 0) *off += r;
    return r;
}

static uint64_t max_size(struct e2fs *fs) {
    uint64_t p = fs->ppb, blocks = 12 + p + p * p + p * p * p;
    uint64_t lim = blocks * fs->bsize;
    return MIN(lim, (uint64_t)2 << 40);
}

static ssize_t e2_write(struct file *fl, const void *buf, size_t n, off_t *off) {
    struct inode *i = fl->inode;
    struct e2inode *ei = EI(i);
    struct e2fs *fs = FS_OF(i);
    if (rdonly(fs)) return -EROFS;
    mutex_lock(&ei->lock);
    if (fl->flags & O_APPEND) *off = i->size;
    if (*off < 0) { mutex_unlock(&ei->lock); return -EINVAL; }
    if ((uint64_t)*off >= max_size(fs)) { mutex_unlock(&ei->lock); return -EFBIG; }
    n = MIN(n, max_size(fs) - *off);
    uint64_t size = i->size;
    ssize_t r = filemap_write(&ei->as, &size, buf, n, *off);
    if (r > 0) {
        *off += r;
        __atomic_store_n(&i->size, size, __ATOMIC_RELEASE);
        i->mtime = i->ctime = now_timespec();
        e2_write_inode(i);
    }
    mutex_unlock(&ei->lock);
    if (r > 0 && (fl->flags & O_SYNC) == O_SYNC) vfs_fsync(i, false);
    return r;
}

static int e2_truncate(struct inode *i, uint64_t size) {
    struct e2inode *ei = EI(i);
    struct e2fs *fs = FS_OF(i);
    if (!S_ISREG(i->mode)) return -EINVAL;
    if (rdonly(fs)) return -EROFS;
    if (size > max_size(fs)) return -EFBIG;
    mutex_lock(&ei->lock);
    uint64_t old = i->size;
    __atomic_store_n(&i->size, size, __ATOMIC_RELEASE);
    if (size < old) {
        mapping_truncate(&ei->as, (size + PAGE_SIZE - 1) / PAGE_SIZE);
        if (size % PAGE_SIZE) {        /* zero the rest of the last page (and so of its blocks) */
            struct page *pg;
            if (!filemap_get_page(&ei->as, size / PAGE_SIZE, false, &pg)) {
                memset((uint8_t *)PHYS_TO_VIRT(page_to_phys(pg)) + size % PAGE_SIZE, 0, PAGE_SIZE - size % PAGE_SIZE);
                pagecache_mark_dirty(pg);
                page_put(pg);
            }
        }
        mutex_lock(&ei->map);
        free_from(ei, (size + fs->bsize - 1) / fs->bsize);
        mutex_unlock(&ei->map);
    }
    i->mtime = i->ctime = now_timespec();
    e2_write_inode(i);
    mutex_unlock(&ei->lock);
    return 0;
}

static int e2_fault_page(struct inode *i, uint64_t pgoff, bool shared, paddr_t *pa) {
    return filemap_fault(&EI(i)->as, pgoff, __atomic_load_n(&i->size, __ATOMIC_RELAXED), shared, pa);
}

/* ------------------------------------------------------------------ directories */
static uint8_t dt_of_mode(uint32_t mode) {
    switch (mode & S_IFMT) {
    case S_IFREG: return 1; case S_IFDIR: return 2; case S_IFCHR: return 3; case S_IFBLK: return 4;
    case S_IFIFO: return 5; case S_IFSOCK: return 6; case S_IFLNK: return 7;
    }
    return 0;
}
static const uint8_t linux_dt[8] = { 0, 8, 4, 2, 6, 1, 12, 10 };

static bool de_ok(struct e2fs *fs, struct e2_dirent *e, uint32_t off) {
    return e->rec_len >= 8 && !(e->rec_len & 3) && off + e->rec_len <= fs->bsize && (uint32_t)e->name_len + 8 <= e->rec_len;
}

struct dloc { uint64_t lblk; uint32_t pblk, off, prev; bool has_prev; uint32_t ino; uint8_t type; };

/* find name in dir; fills loc */
static int dir_find(struct inode *dir, const char *name, struct dloc *loc) {
    struct e2fs *fs = FS_OF(dir);
    size_t nl = strlen(name);
    if (nl > 255) return -ENAMETOOLONG;
    uint64_t nblk = dir->size / fs->bsize;
    for (uint64_t l = 0; l < nblk; l++) {
        uint32_t pb;
        if (bmap(EI(dir), l, false, &pb, nullptr) || !pb) continue;
        struct buf b;
        if (bread(fs->dev, pb, fs->bsize, &b)) return -EIO;
        uint32_t prev = 0; bool hp = false;
        for (uint32_t off = 0; off < fs->bsize;) {
            struct e2_dirent *e = (struct e2_dirent *)(b.data + off);
            if (!de_ok(fs, e, off)) { brelse(&b); pr_warn("ext2: corrupt directory %lu block %lu\n", dir->ino, l); return -EIO; }
            if (e->inode && e->name_len == nl && !memcmp(e->name, name, nl)) {
                *loc = (struct dloc){ l, pb, off, prev, hp, e->inode, e->type };
                brelse(&b);
                return 0;
            }
            prev = off; hp = true;
            off += e->rec_len;
        }
        brelse(&b);
    }
    return -ENOENT;
}

static void dir_touch(struct inode *dir) {
    dir->mtime = dir->ctime = now_timespec();
    EI(dir)->flags &= ~EXT2_INDEX_FL;
    e2_write_inode(dir);
}

static int dir_add(struct inode *dir, const char *name, uint32_t ino, uint32_t mode) {
    struct e2fs *fs = FS_OF(dir);
    size_t nl = strlen(name);
    if (nl > 255) return -ENAMETOOLONG;
    uint32_t need = (8 + (uint32_t)nl + 3) & ~3u;
    uint8_t type = (RAWSB(fs)->feature_incompat & INCOMPAT_FILETYPE) ? dt_of_mode(mode) : 0;
    uint64_t nblk = dir->size / fs->bsize;
    for (uint64_t l = 0; l <= nblk; l++) {
        uint32_t pb;
        struct buf b;
        bool fresh = l == nblk;
        int r;
        if (fresh) {
            mutex_lock(&EI(dir)->map);
            r = bmap(EI(dir), l, true, &pb, nullptr);
            mutex_unlock(&EI(dir)->map);
            if (r) return r;
            if ((r = bget_new(fs->dev, pb, fs->bsize, &b))) return r;
            struct e2_dirent *e = (struct e2_dirent *)b.data;
            e->inode = 0; e->rec_len = (uint16_t)fs->bsize; e->name_len = 0; e->type = 0;
            dir->size += fs->bsize;
        } else {
            if (bmap(EI(dir), l, false, &pb, nullptr) || !pb) continue;
            if ((r = bread(fs->dev, pb, fs->bsize, &b))) return r;
        }
        for (uint32_t off = 0; off < fs->bsize;) {
            struct e2_dirent *e = (struct e2_dirent *)(b.data + off);
            if (!de_ok(fs, e, off)) { brelse(&b); return -EIO; }
            uint32_t used = e->inode ? (8 + e->name_len + 3) & ~3u : 0;
            if (e->rec_len - used >= need) {
                struct e2_dirent *n = e;
                if (used) {
                    n = (struct e2_dirent *)(b.data + off + used);
                    n->rec_len = (uint16_t)(e->rec_len - used);
                    e->rec_len = (uint16_t)used;
                }
                n->inode = ino; n->name_len = (uint8_t)nl; n->type = type;
                memcpy(n->name, name, nl);
                bdirty(&b);
                brelse(&b);
                dir_touch(dir);
                return 0;
            }
            off += e->rec_len;
        }
        if (fresh) { bdirty(&b); brelse(&b); dir_touch(dir); return -EIO; }
        brelse(&b);
    }
    return -ENOSPC;
}

static int dir_remove(struct inode *dir, struct dloc *loc) {
    struct e2fs *fs = FS_OF(dir);
    struct buf b;
    int r = bread(fs->dev, loc->pblk, fs->bsize, &b);
    if (r) return r;
    struct e2_dirent *e = (struct e2_dirent *)(b.data + loc->off);
    if (loc->has_prev) ((struct e2_dirent *)(b.data + loc->prev))->rec_len += e->rec_len;
    else e->inode = 0;
    bdirty(&b);
    brelse(&b);
    dir_touch(dir);
    return 0;
}

/* point an existing entry (or "..") at another inode */
static int dir_retarget(struct inode *dir, struct dloc *loc, uint32_t ino, uint32_t mode) {
    struct e2fs *fs = FS_OF(dir);
    struct buf b;
    int r = bread(fs->dev, loc->pblk, fs->bsize, &b);
    if (r) return r;
    struct e2_dirent *e = (struct e2_dirent *)(b.data + loc->off);
    e->inode = ino;
    if (RAWSB(fs)->feature_incompat & INCOMPAT_FILETYPE) e->type = dt_of_mode(mode);
    bdirty(&b);
    brelse(&b);
    dir_touch(dir);
    return 0;
}

static bool dir_empty(struct inode *dir) {
    struct e2fs *fs = FS_OF(dir);
    uint64_t nblk = dir->size / fs->bsize;
    for (uint64_t l = 0; l < nblk; l++) {
        uint32_t pb;
        if (bmap(EI(dir), l, false, &pb, nullptr) || !pb) continue;
        struct buf b;
        if (bread(fs->dev, pb, fs->bsize, &b)) return false;
        for (uint32_t off = 0; off < fs->bsize;) {
            struct e2_dirent *e = (struct e2_dirent *)(b.data + off);
            if (!de_ok(fs, e, off)) break;
            if (e->inode && !(e->name_len == 1 && e->name[0] == '.') && !(e->name_len == 2 && e->name[0] == '.' && e->name[1] == '.')) {
                brelse(&b);
                return false;
            }
            off += e->rec_len;
        }
        brelse(&b);
    }
    return true;
}

static int e2_iterate(struct inode *dir, uint64_t *pos, filldir_t fill, void *ctx) {
    struct e2fs *fs = FS_OF(dir);
    vfs_ns_lock();
    bool ft = RAWSB(fs)->feature_incompat & INCOMPAT_FILETYPE;
    while (*pos < dir->size) {
        uint64_t l = *pos / fs->bsize;
        uint32_t want = (uint32_t)(*pos % fs->bsize), pb;
        struct buf b;
        if (bmap(EI(dir), l, false, &pb, nullptr) || !pb || bread(fs->dev, pb, fs->bsize, &b)) { *pos = (l + 1) * fs->bsize; continue; }
        bool stop = false;
        for (uint32_t off = 0; off < fs->bsize;) {
            struct e2_dirent *e = (struct e2_dirent *)(b.data + off);
            if (!de_ok(fs, e, off)) break;
            if (off >= want && e->inode) {
                unsigned t = ft && e->type < 8 ? linux_dt[e->type] : 0;
                if (fill(ctx, e->name, e->name_len, e->inode, t)) { *pos = l * fs->bsize + off; stop = true; break; }
            }
            off += e->rec_len;
        }
        brelse(&b);
        if (stop) break;
        *pos = (l + 1) * fs->bsize;
    }
    vfs_ns_unlock();
    return 0;
}

static int e2_lookup(struct inode *dir, const char *name, struct inode **out) {
    struct dloc loc;
    int r = dir_find(dir, name, &loc);
    if (r) return r;
    struct inode *c;
    if ((r = e2_iget(FS_OF(dir), loc.ino, &c))) return r;
    if (S_ISDIR(c->mode) && !c->parent && c != dir) { iget(dir); c->parent = dir; }
    *out = c;
    return 0;
}

/* new on-disk inode (referenced, cached, written) */
static int new_inode(struct inode *dir, uint32_t mode, uint64_t rdev, struct inode **out) {
    struct e2fs *fs = FS_OF(dir);
    if (rdonly(fs)) return -EROFS;
    uint32_t ino;
    int r = ialloc(fs, (uint32_t)((dir->ino - 1) / fs->ipg), S_ISDIR(mode), &ino);
    if (r) return r;
    struct e2inode *ei = kzalloc(sizeof *ei);
    if (!ei) { ifree(fs, ino, S_ISDIR(mode)); return -ENOMEM; }
    struct inode *i = &ei->v;
    inode_init(i, mode);
    i->ino = ino;
    i->uid = curproc ? curproc->euid : 0;
    i->gid = curproc ? curproc->egid : 0;
    if (dir->mode & 02000) { i->gid = dir->gid; if (S_ISDIR(mode)) i->mode |= 02000; }
    i->nlink = 1;
    i->rdev = rdev;
    ei->generation = ++fs->gen;
    if (S_ISCHR(mode) || S_ISBLK(mode)) {
        if (MAJOR(rdev) < 256 && MINOR(rdev) < 256) ei->blk[0] = (uint32_t)rdev;
        else ei->blk[1] = (uint32_t)((MINOR(rdev) & 0xff) | (MAJOR(rdev) << 8) | ((MINOR(rdev) & ~0xffu) << 12));
    }
    e2_setup(fs, ei);
    icache_insert(&fs->sb, i);
    e2_write_inode(i);
    *out = i;
    return 0;
}

static void drop_new(struct inode *i) { i->nlink = 0; iput(i); }

static int e2_create(struct inode *dir, const char *name, uint32_t mode, uint64_t rdev, struct inode **out) {
    struct e2fs *fs = FS_OF(dir);
    if (strlen(name) > 255) return -ENAMETOOLONG;
    if (S_ISDIR(mode) && dir->nlink >= 65000) return -EMLINK;
    struct inode *i;
    int r = new_inode(dir, mode, rdev, &i);
    if (r) return r;
    if (S_ISDIR(mode)) {
        uint32_t pb;
        struct buf b;
        if ((r = bmap(EI(i), 0, true, &pb, nullptr)) || (r = bget_new(fs->dev, pb, fs->bsize, &b))) { drop_new(i); return r; }
        struct e2_dirent *d1 = (struct e2_dirent *)b.data, *d2 = (struct e2_dirent *)(b.data + 12);
        bool ft = RAWSB(fs)->feature_incompat & INCOMPAT_FILETYPE;
        *d1 = (struct e2_dirent){ (uint32_t)i->ino, 12, 1, ft ? 2 : 0 }; d1->name[0] = '.';
        *d2 = (struct e2_dirent){ (uint32_t)dir->ino, (uint16_t)(fs->bsize - 12), 2, ft ? 2 : 0 }; d2->name[0] = d2->name[1] = '.';
        bdirty(&b);
        brelse(&b);
        i->size = fs->bsize;
        i->nlink = 2;
        e2_write_inode(i);
        iget(dir);
        i->parent = dir;
    }
    if ((r = dir_add(dir, name, (uint32_t)i->ino, mode))) {
        if (S_ISDIR(mode)) i->nlink = 0;
        drop_new(i);
        return r;
    }
    if (S_ISDIR(mode)) { dir->nlink++; e2_write_inode(dir); }
    *out = i;
    return 0;
}

static int e2_unlink(struct inode *dir, const char *name, bool rmdir) {
    struct e2fs *fs = FS_OF(dir);
    if (rdonly(fs)) return -EROFS;
    struct dloc loc;
    int r = dir_find(dir, name, &loc);
    if (r) return r;
    struct inode *c;
    if ((r = e2_iget(fs, loc.ino, &c))) return r;
    if (rmdir) {
        if (!S_ISDIR(c->mode)) r = -ENOTDIR;
        else if (c->mounted) r = -EBUSY;
        else if (!dir_empty(c)) r = -ENOTEMPTY;
    } else if (S_ISDIR(c->mode)) r = -EISDIR;
    if (!r) r = dir_remove(dir, &loc);
    if (!r) {
        c->ctime = now_timespec();
        if (rmdir) {
            c->nlink = 0;
            if (dir->nlink > 2) dir->nlink--;
            e2_write_inode(dir);
        } else if (c->nlink) c->nlink--;
        e2_write_inode(c);
    }
    iput(c);
    return r;
}

static int e2_symlink(struct inode *dir, const char *name, const char *target) {
    struct e2fs *fs = FS_OF(dir);
    size_t len = strlen(target);
    if (len >= fs->bsize || len >= 4096) return -ENAMETOOLONG;
    struct inode *i;
    int r = new_inode(dir, S_IFLNK | 0777, 0, &i);
    if (r) return r;
    struct e2inode *ei = EI(i);
    if (len < sizeof ei->blk) memcpy(ei->blk, target, len);
    else {
        uint32_t pb;
        struct buf b;
        if ((r = bmap(ei, 0, true, &pb, nullptr)) || (r = bget_new(fs->dev, pb, fs->bsize, &b))) { drop_new(i); return r; }
        memcpy(b.data, target, len);
        bdirty(&b);
        brelse(&b);
    }
    i->size = len;
    e2_write_inode(i);
    if ((r = dir_add(dir, name, (uint32_t)i->ino, i->mode))) { drop_new(i); return r; }
    iput(i);
    return 0;
}

static int e2_readlink(struct inode *i, char *buf, size_t size) {
    struct e2fs *fs = FS_OF(i);
    size_t l = MIN(i->size, size);
    if (!has_blocks(i)) { memcpy(buf, EI(i)->blk, MIN(l, sizeof EI(i)->blk)); return (int)MIN(l, sizeof EI(i)->blk); }
    uint32_t pb;
    struct buf b;
    if (bmap(EI(i), 0, false, &pb, nullptr) || !pb || bread(fs->dev, pb, fs->bsize, &b)) return -EIO;
    l = MIN(l, fs->bsize);
    memcpy(buf, b.data, l);
    brelse(&b);
    return (int)l;
}

static int e2_link(struct inode *dir, const char *name, struct inode *target) {
    if (target->sb != dir->sb) return -EXDEV;
    if (rdonly(FS_OF(dir))) return -EROFS;
    if (target->nlink >= 65000) return -EMLINK;
    int r = dir_add(dir, name, (uint32_t)target->ino, target->mode);
    if (r) return r;
    target->nlink++;
    target->ctime = now_timespec();
    e2_write_inode(target);
    return 0;
}

static bool is_ancestor(struct inode *a, struct inode *d) {
    for (int n = 0; d && n < 4096; n++) {
        if (d == a) return true;
        if (d->parent == d || d == d->sb->root) break;
        d = d->parent;
    }
    return false;
}

static int e2_rename(struct inode *od, const char *on, struct inode *nd, const char *nn) {
    struct e2fs *fs = FS_OF(od);
    if (rdonly(fs)) return -EROFS;
    if (strlen(nn) > 255) return -ENAMETOOLONG;
    struct dloc ol, nlc;
    int r = dir_find(od, on, &ol);
    if (r) return r;
    if (od == nd && !strcmp(on, nn)) return 0;
    struct inode *src, *tgt = nullptr;
    if ((r = e2_iget(fs, ol.ino, &src))) return r;
    bool isdir = S_ISDIR(src->mode);
    if (isdir && is_ancestor(src, nd)) { iput(src); return -EINVAL; }
    r = dir_find(nd, nn, &nlc);
    if (!r) {
        if ((r = e2_iget(fs, nlc.ino, &tgt))) { iput(src); return r; }
        if (tgt == src) { iput(tgt); iput(src); return 0; }
        if (S_ISDIR(tgt->mode) != isdir) r = isdir ? -ENOTDIR : -EISDIR;
        else if (isdir && tgt->mounted) r = -EBUSY;
        else if (isdir && !dir_empty(tgt)) r = -ENOTEMPTY;
        if (!r) r = dir_retarget(nd, &nlc, (uint32_t)src->ino, src->mode);
        if (r) { iput(tgt); iput(src); return r; }
        tgt->ctime = now_timespec();
        if (isdir) { tgt->nlink = 0; if (nd->nlink > 2) nd->nlink--; }
        else if (tgt->nlink) tgt->nlink--;
        e2_write_inode(tgt);
    } else if (r == -ENOENT) {
        if (isdir && nd != od && nd->nlink >= 65000) { iput(src); return -EMLINK; }
        if ((r = dir_add(nd, nn, (uint32_t)src->ino, src->mode))) { iput(src); return r; }
    } else { iput(src); return r; }
    /* the old entry may have moved if od == nd and the add split a record in its block */
    if (!(r = dir_find(od, on, &ol)) && ol.ino == src->ino) dir_remove(od, &ol);
    if (isdir && od != nd) {
        struct dloc dd;
        if (!dir_find(src, "..", &dd)) dir_retarget(src, &dd, (uint32_t)nd->ino, S_IFDIR);
        if (od->nlink > 2) od->nlink--;
        nd->nlink++;
        e2_write_inode(od);
        e2_write_inode(nd);
        struct inode *op = src->parent;
        iget(nd);
        src->parent = nd;
        if (op && op != src) iput(op);
    }
    src->ctime = now_timespec();
    e2_write_inode(src);
    if (tgt) iput(tgt);
    iput(src);
    return 0;
}

/* last reference: free on disk if unlinked, then the in-core inode */
static void e2_evict(struct inode *i) {
    struct e2fs *fs = FS_OF(i);
    struct e2inode *ei = EI(i);
    mapping_truncate(&ei->as, 0);
    if (i->nlink == 0 && !rdonly(fs)) {
        mutex_lock(&ei->map);
        free_from(ei, 0);
        mutex_unlock(&ei->map);
        i->size = 0;
        ei->dtime = (uint32_t)now_timespec().tv_sec;
        e2_write_inode(i);
        ifree(fs, (uint32_t)i->ino, S_ISDIR(i->mode));
    }
    struct inode *p = i->parent;
    kfree(ei);
    if (p && p != i) iput(p);
}

/* ------------------------------------------------------------------ superblock ops */
static int e2_sync_fs(struct super_block *sb) {
    struct e2fs *fs = E2FS(sb);
    if (rdonly(fs)) return 0;
    mutex_lock(&fs->alloc);
    int r = fs->sb_dirty ? sb_write(fs) : 0;
    mutex_unlock(&fs->alloc);
    return r;
}

static int e2_statfs(struct super_block *sb, struct kstatfs *st) {
    struct e2fs *fs = E2FS(sb);
    struct e2_sb *s = RAWSB(fs);
    st->type = EXT2_MAGIC;
    st->bsize = fs->bsize;
    st->blocks = s->blocks_count - fs->first_data;
    st->bfree = s->free_blocks;
    st->bavail = s->free_blocks > s->r_blocks_count ? s->free_blocks - s->r_blocks_count : 0;
    st->files = s->inodes_count;
    st->ffree = s->free_inodes;
    st->namelen = 255;
    st->flags = rdonly(fs) ? 1 : 0;
    return 0;
}

static int e2_remount(struct super_block *sb, uint32_t flags) {
    struct e2fs *fs = E2FS(sb);
    bool ro = flags & SB_RDONLY;
    if (ro == rdonly(fs)) return 0;
    if (ro) {
        vfs_sync_all();
        mutex_lock(&fs->alloc);
        RAWSB(fs)->state |= STATE_VALID;
        sb_write(fs);
        mutex_unlock(&fs->alloc);
        blk_sync(fs->dev);
        sb->flags |= SB_RDONLY;
    } else {
        if (fs->dev->readonly) return -EROFS;
        sb->flags &= ~SB_RDONLY;
        mutex_lock(&fs->alloc);
        RAWSB(fs)->state &= ~STATE_VALID;
        sb_write(fs);
        mutex_unlock(&fs->alloc);
    }
    return 0;
}

static void e2_put_super(struct super_block *sb) {
    struct e2fs *fs = E2FS(sb);
    if (!rdonly(fs)) {
        mutex_lock(&fs->alloc);
        RAWSB(fs)->state |= STATE_VALID;
        sb_write(fs);
        mutex_unlock(&fs->alloc);
    }
    blk_sync(fs->dev);
    if (sb->root) { struct inode *r = sb->root; sb->root = nullptr; r->parent = nullptr; mapping_truncate(&EI(r)->as, 0); kfree(EI(r)); }
    mapping_truncate(fs->dev->inode->mapping, 0);     /* drop the device cache */
    kfree(fs->gdt);
    kfree(sb->ihash);
    kfree(fs);
}

static const struct super_ops e2_sops = {
    .write_inode = e2_write_inode, .evict_inode = e2_evict, .sync_fs = e2_sync_fs, .statfs = e2_statfs,
    .remount = e2_remount, .put_super = e2_put_super,
};

static int e2_mount(struct blkdev *dev, uint32_t flags, const char *data, struct super_block **out) {
    struct buf b;
    int r = bread(dev, 1, 1024, &b);
    if (r) return r;
    struct e2fs *fs = kzalloc(sizeof *fs);
    if (!fs) { brelse(&b); return -ENOMEM; }
    memcpy(fs->raw, b.data, 1024);
    brelse(&b);
    struct e2_sb *s = RAWSB(fs);
    if (s->magic != EXT2_MAGIC || s->log_block_size > 2 || !s->blocks_per_group || !s->inodes_per_group) {
        kfree(fs);
        return -EINVAL;
    }
    fs->rev = s->rev_level;
    if (fs->rev >= 1 && (s->feature_incompat & ~INCOMPAT_FILETYPE)) {
        pr_err("ext2: %s: unsupported incompatible features %#x\n", dev->name, s->feature_incompat & ~INCOMPAT_FILETYPE);
        kfree(fs);
        return -EINVAL;
    }
    if (fs->rev >= 1 && (s->feature_ro_compat & ~(RO_SPARSE | RO_LARGE | RO_BTREE))) {
        pr_warn("ext2: %s: unknown ro_compat features %#x, mounting read-only\n", dev->name, s->feature_ro_compat);
        flags |= SB_RDONLY;
    }
    if (dev->readonly) flags |= SB_RDONLY;
    fs->dev = dev;
    fs->bsize = 1024u << s->log_block_size;
    fs->ppb = fs->bsize / 4;
    fs->spb = fs->bsize / SECTOR_SIZE;
    fs->ipg = s->inodes_per_group;
    fs->bpg = s->blocks_per_group;
    fs->first_data = s->first_data_block;
    fs->isize = fs->rev >= 1 ? s->inode_size : 128;
    fs->first_ino = fs->rev >= 1 ? s->first_ino : 11;
    fs->ngroups = (s->blocks_count - fs->first_data + fs->bpg - 1) / fs->bpg;
    if (fs->isize < 128 || fs->isize > fs->bsize || (fs->isize & (fs->isize - 1)) || !fs->ngroups ||
        (uint64_t)s->blocks_count * fs->spb > dev->nsectors) {
        pr_err("ext2: %s: bad geometry\n", dev->name);
        kfree(fs);
        return -EINVAL;
    }
    size_t gbytes = (size_t)fs->ngroups * sizeof(struct e2_gd);
    fs->gdt = kmalloc(gbytes + fs->bsize);
    for (size_t off = 0; off < gbytes; off += fs->bsize) {
        if ((r = bread(dev, fs->first_data + 1 + off / fs->bsize, fs->bsize, &b))) { kfree(fs->gdt); kfree(fs); return r; }
        memcpy((uint8_t *)fs->gdt + off, b.data, MIN((size_t)fs->bsize, gbytes - off));
        brelse(&b);
    }
    mutex_init(&fs->alloc, &alloc_class);
    mutex_init(&fs->icache, &icache_class);
    sb_init(&fs->sb, &e2_sops, "ext2");
    fs->sb.bdev = dev;
    fs->sb.dev = dev->rdev;
    fs->sb.flags = (flags & SB_RDONLY) | SB_DCACHE;
    fs->sb.priv = fs;
    fs->gen = (uint32_t)time_ns();
    struct inode *root;
    if ((r = e2_iget(fs, ROOT_INO, &root)) || !S_ISDIR(root->mode)) {
        pr_err("ext2: %s: no root directory\n", dev->name);
        kfree(fs->gdt); kfree(fs->sb.ihash); kfree(fs);
        return r ? r : -EINVAL;
    }
    fs->sb.root = root;
    root->parent = root;
    if (!rdonly(fs)) {
        if (!(s->state & STATE_VALID)) pr_warn("ext2: %s: not cleanly unmounted, run e2fsck\n", dev->name);
        s->state &= ~STATE_VALID;
        s->mnt_count++;
        s->mtime = (uint32_t)now_timespec().tv_sec;
        mutex_lock(&fs->alloc);
        sb_write(fs);
        mutex_unlock(&fs->alloc);
    }
    pr_info("ext2: %s: %u blocks of %u bytes, %u groups, %u/%u inodes free%s\n", dev->name, s->blocks_count, fs->bsize,
            fs->ngroups, s->free_inodes, s->inodes_count, rdonly(fs) ? ", read-only" : "");
    *out = &fs->sb;
    return 0;
}

static const struct inode_ops e2_iops = {
    .lookup = e2_lookup, .create = e2_create, .unlink = e2_unlink, .symlink = e2_symlink,
    .readlink = e2_readlink, .link = e2_link, .rename = e2_rename, .truncate = e2_truncate,
    .iterate = e2_iterate,
};
static unsigned e2_poll(struct file *f) { return POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM; }
static const struct file_ops e2_fops = { .nobkl = true, .read = e2_read, .write = e2_write, .poll = e2_poll, .fault_page = e2_fault_page };
static const struct file_ops e2_dir_fops = { .poll = e2_poll };
static const struct file_ops e2_lnk_fops = { .poll = e2_poll };

static const struct fs_type ext2_type = { "ext2", true, e2_mount };
void ext2_init(void) { fs_register(&ext2_type); }
