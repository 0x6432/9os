#pragma once
/*
 * Unified page cache (M30, mm/filemap.c). Every cached file — tmpfs (backed by the initramfs
 * image or nothing), ext2 files and directories, and block devices themselves (the "buffer
 * cache": filesystem metadata lives in the block device's mapping) — keeps its pages in a
 * struct address_space: a 64-ary radix tree of struct page indexed by page offset.
 *
 * Page states (struct page uflags): PGU_CACHE (in a mapping), PGU_UPTODATE (contents valid),
 * PGU_LOCKED (I/O in flight: wait with filemap_wait_page), PGU_DIRTY (newer than the backing
 * store: never reclaimed; written back by the "writeback" thread, fsync/sync, or never for
 * mappings without ->writepage, i.e. tmpfs), PGU_LRU/PGU_REFERENCED (global clean-page LRU).
 *
 * Locking: m->lock (IRQ-off spinlock, LR_PAGECACHE) guards the tree; the fault path (under
 * mm->lock) only looks pages up and fills them inline when the mapping's fill is atomic
 * (tmpfs). Otherwise it returns -EAGAIN and the fault is retried after
 * filemap_fault_prepare() read the page with no spinlocks held. Readers hold a page
 * reference instead of a mutex, writers serialise on the owner's inode mutex.
 */
#include <kernel/types.h>
#include <kernel/spinlock.h>
#include <kernel/list.h>
#include <kernel/pmm.h>

struct inode;
struct address_space;

enum { PGU_UPTODATE = 1 << 4, PGU_LOCKED = 1 << 5, PGU_ERROR = 1 << 6 };

struct aspace_ops {
    /* Start reading page pg (index pg->index, PGU_LOCKED set). Completion (possibly from an
     * interrupt) calls filemap_read_done(pg, err). Null: pages start zero-filled. */
    int (*readpage)(struct address_space *m, struct page *pg);
    /* Write page pg back synchronously (may sleep). Null: dirty pages stay in memory. */
    int (*writepage)(struct address_space *m, struct page *pg);
    /* Before a write into [from, to) of page idx (may sleep): allocate backing blocks. */
    int (*prepare_write)(struct address_space *m, uint64_t idx, unsigned from, unsigned to);
};

struct aspace_node;
struct address_space {
    struct inode *host;
    const struct aspace_ops *ops;
    spinlock_t lock;
    struct aspace_node *root;
    unsigned height;              /* tree covers 64^height pages */
    uint64_t nrpages, nrdirty;
    bool atomic_fill;             /* readpage never sleeps: the fault path may fill inline */
    bool no_lru;                  /* never reclaim clean pages (e.g. unbacked tmpfs) */
    struct list_node dirty_node;  /* on the global dirty list while nrdirty > 0 */
    bool on_dirty_list;
    uint64_t wb_errors;
};

void mapping_init(struct address_space *m, struct inode *host, const struct aspace_ops *ops);
/* drop every page at index >= from (waits for I/O); from == 0 when the inode goes away */
void mapping_truncate(struct address_space *m, uint64_t from);
/* cached page with a reference (no I/O) or null */
struct page *filemap_find(struct address_space *m, uint64_t idx);
/* page with a reference, read if needed (sleeps). create: zero-filled new page without I/O
 * (the caller overwrites it entirely or it lies past the end of the backing data). */
int filemap_get_page(struct address_space *m, uint64_t idx, bool create, struct page **out);
void filemap_readahead(struct address_space *m, uint64_t idx, unsigned n);
void filemap_read_done(struct page *pg, int err);   /* readpage completion (any context) */
void filemap_wait_page(struct page *pg);            /* sleep while PGU_LOCKED */
/* fault path (mm->lock held): referenced uptodate page, -EAGAIN (call filemap_fault_prepare
 * with no locks held and retry), -ENXIO past EOF for private mappings, -ENOMEM */
int filemap_fault(struct address_space *m, uint64_t idx, uint64_t size, bool shared, paddr_t *pa);
int filemap_fault_prepare(struct address_space *m, uint64_t idx);

/* generic file I/O on top of a mapping (caller holds the inode's mutex for writes) */
ssize_t filemap_read(struct address_space *m, uint64_t size, void *buf, size_t n, uint64_t off);
ssize_t filemap_write(struct address_space *m, uint64_t *size, const void *buf, size_t n, uint64_t off);
/* zero the tail of the page containing size (truncate to an unaligned length) */
void filemap_zero_tail(struct address_space *m, uint64_t size);

void pagecache_mark_dirty(struct page *pg);
void pagecache_mark_referenced(struct page *pg);
/* write back dirty pages of [start, end) pages (end == UINT64_MAX: all); wait: sync */
int filemap_writeback(struct address_space *m, uint64_t start, uint64_t end);
int writeback_all(void);                  /* sync(2): every dirty mapping */
void writeback_kick(void);                /* wake the writeback thread (memory pressure) */
void writeback_init(void);
uint64_t pagecache_reclaim(uint64_t want);

extern uint64_t pagecache_pages, pagecache_lru_pages, pagecache_filled, pagecache_reclaimed,
                pagecache_dirty, pagecache_written, pagecache_reads, pagecache_hits;

/* copy helpers: kernel addresses are kernel buffers (sendfile, exec, ext2 internals) */
int pc_copy_out(void *dst, const void *src, size_t n);
int pc_copy_in(void *dst, const void *src, size_t n);
