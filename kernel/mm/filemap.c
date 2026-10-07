/* Unified page cache (M30): radix-tree mappings, read/write/fault/truncate, LRU reclaim and
 * write-back. See kernel/pagecache.h. */
#include <kernel/pagecache.h>
#include <kernel/vfs.h>
#include <kernel/uaccess.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/boot.h>
#include <kernel/mm.h>
#include <kernel/time.h>

static const struct lock_class pc_class = { "pagecache", LR_PAGECACHE, false };
static const struct lock_class lru_class = { "lru", LR_LRU, false };
static const struct lock_class dirty_class = { "dirty_list", LR_DIRTYLIST, false };

uint64_t pagecache_pages, pagecache_lru_pages, pagecache_filled, pagecache_reclaimed,
         pagecache_dirty, pagecache_written, pagecache_reads, pagecache_hits;

static struct list_node lru = LIST_INIT(lru);
static spinlock_t lru_lock = SPINLOCK_INIT_CLASS(&lru_class);
static struct list_node dirty_list = LIST_INIT(dirty_list);
static spinlock_t dirty_lock = SPINLOCK_INIT_CLASS(&dirty_class);
static struct wait_queue page_wq = WAIT_QUEUE_INIT(page_wq);
static struct wait_queue wb_wq = WAIT_QUEUE_INIT(wb_wq);
static int wb_kicked;

int pc_copy_out(void *dst, const void *src, size_t n) {
    if ((vaddr_t)dst >= USER_TOP) { memcpy(dst, src, n); return 0; }
    return copy_to_user(dst, src, n);
}
int pc_copy_in(void *dst, const void *src, size_t n) {
    if ((vaddr_t)src >= USER_TOP) { memcpy(dst, src, n); return 0; }
    return copy_from_user(dst, src, n);
}

static uint64_t m_lock(struct address_space *m) { uint64_t f = arch_irq_save(); spin_lock_ipi(&m->lock); return f; }
static void m_unlock(struct address_space *m, uint64_t f) { spin_unlock(&m->lock); arch_irq_restore(f); }

/* ------------------------------------------------------------------ radix tree */
#define RT_SHIFT 6
#define RT_SLOTS 64
struct aspace_node { void *slot[RT_SLOTS]; unsigned count; };

static uint64_t rt_cap(unsigned h) { return h >= 10 ? UINT64_MAX : 1ull << (RT_SHIFT * h); }

static struct page *rt_lookup(struct address_space *m, uint64_t idx) {
    if (!m->root || idx >= rt_cap(m->height)) return nullptr;
    struct aspace_node *n = m->root;
    for (unsigned h = m->height; h > 1; h--) {
        n = n->slot[(idx >> (RT_SHIFT * (h - 1))) & (RT_SLOTS - 1)];
        if (!n) return nullptr;
    }
    return n->slot[idx & (RT_SLOTS - 1)];
}

static int rt_insert(struct address_space *m, uint64_t idx, struct page *pg) {
    if (!m->root) { m->root = kzalloc(sizeof *m->root); if (!m->root) return -ENOMEM; m->height = 1; }
    while (idx >= rt_cap(m->height)) {
        struct aspace_node *n = kzalloc(sizeof *n);
        if (!n) return -ENOMEM;
        n->slot[0] = m->root; n->count = 1;
        m->root = n; m->height++;
    }
    struct aspace_node *n = m->root;
    for (unsigned h = m->height; h > 1; h--) {
        unsigned s = (idx >> (RT_SHIFT * (h - 1))) & (RT_SLOTS - 1);
        if (!n->slot[s]) {
            struct aspace_node *c = kzalloc(sizeof *c);
            if (!c) return -ENOMEM;
            n->slot[s] = c; n->count++;
        }
        n = n->slot[s];
    }
    unsigned s = idx & (RT_SLOTS - 1);
    if (n->slot[s]) return -EEXIST;
    n->slot[s] = pg; n->count++;
    return 0;
}

static bool rt_delete_rec(struct aspace_node *n, unsigned h, uint64_t idx) {
    unsigned s = (idx >> (RT_SHIFT * (h - 1))) & (RT_SLOTS - 1);
    if (!n->slot[s]) return false;
    if (h == 1) { n->slot[s] = nullptr; n->count--; }
    else if (rt_delete_rec(n->slot[s], h - 1, idx)) { kfree(n->slot[s]); n->slot[s] = nullptr; n->count--; }
    return n->count == 0;
}
static void rt_delete(struct address_space *m, uint64_t idx) {
    if (!m->root || idx >= rt_cap(m->height)) return;
    if (rt_delete_rec(m->root, m->height, idx)) { kfree(m->root); m->root = nullptr; m->height = 0; }
}

/* collect up to max pages with index >= *start (in order), each with a reference */
static int rt_gang_rec(struct aspace_node *n, unsigned h, uint64_t base, uint64_t start, uint64_t end,
                       struct page **out, int max, int cnt) {
    uint64_t span = rt_cap(h - 1);
    for (unsigned s = 0; s < RT_SLOTS && cnt < max; s++) {
        uint64_t lo = base + s * span;
        if (!n->slot[s] || lo + span <= start || lo >= end) continue;
        if (h == 1) { struct page *pg = n->slot[s]; page_ref_inc(pg); out[cnt++] = pg; }
        else cnt = rt_gang_rec(n->slot[s], h - 1, lo, start, end, out, max, cnt);
    }
    return cnt;
}
static int rt_gang(struct address_space *m, uint64_t start, uint64_t end, struct page **out, int max) {
    if (!m->root || start >= rt_cap(m->height)) return 0;
    return rt_gang_rec(m->root, m->height, 0, start, end, out, max, 0);
}

/* ------------------------------------------------------------------ LRU / dirty accounting */
static void lru_add(struct page *pg) {
    struct address_space *m = pg->mapping;
    if (!m || m->no_lru) return;
    uint64_t f = arch_irq_save();
    spin_lock_ipi(&lru_lock);
    if (page_uflag_test(pg, PGU_CACHE) && !page_uflag_test(pg, PGU_LRU | PGU_DIRTY | PGU_LOCKED) &&
        page_uflag_test(pg, PGU_UPTODATE)) {
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
static void lru_del(struct page *pg) {
    uint64_t f = arch_irq_save();
    spin_lock_ipi(&lru_lock);
    lru_del_locked(pg);
    spin_unlock(&lru_lock);
    arch_irq_restore(f);
}

void pagecache_mark_referenced(struct page *pg) {
    if (!page_uflag_test(pg, PGU_REFERENCED)) page_uflag_set(pg, PGU_REFERENCED);
}

void pagecache_mark_dirty(struct page *pg) {
    if (page_uflag_test(pg, PGU_DIRTY)) return;
    uint16_t old = __atomic_fetch_or(&pg->uflags, PGU_DIRTY, __ATOMIC_ACQ_REL);
    if (old & PGU_DIRTY) return;
    lru_del(pg);
    struct address_space *m = pg->mapping;
    if (!m || !(old & PGU_CACHE)) return;
    __atomic_fetch_add(&pagecache_dirty, 1, __ATOMIC_RELAXED);
    uint64_t f = arch_irq_save();
    spin_lock_ipi(&dirty_lock);
    m->nrdirty++;
    if (!m->on_dirty_list && !m->no_writeback && m->ops && m->ops->writepage) { m->on_dirty_list = true; list_add_tail(&dirty_list, &m->dirty_node); }
    spin_unlock(&dirty_lock);
    arch_irq_restore(f);
}

/* clear the dirty bit before writing the page (a write during the I/O re-dirties it) */
static bool clear_dirty(struct address_space *m, struct page *pg) {
    uint16_t old = __atomic_fetch_and(&pg->uflags, (uint16_t)~PGU_DIRTY, __ATOMIC_ACQ_REL);
    if (!(old & PGU_DIRTY)) return false;
    __atomic_fetch_sub(&pagecache_dirty, 1, __ATOMIC_RELAXED);
    uint64_t f = arch_irq_save();
    spin_lock_ipi(&dirty_lock);
    if (m->nrdirty) m->nrdirty--;
    spin_unlock(&dirty_lock);
    arch_irq_restore(f);
    return true;
}

/* ------------------------------------------------------------------ mapping lifetime */
void mapping_init(struct address_space *m, struct inode *host, const struct aspace_ops *ops) {
    memset(m, 0, sizeof *m);
    m->host = host;
    m->ops = ops;
    spin_lock_init_class(&m->lock, &pc_class);
    list_init(&m->dirty_node);
}

/* remove pg from m (lock held); drops the cache's reference */
static void cache_remove_locked(struct address_space *m, struct page *pg) {
    rt_delete(m, pg->index);
    m->nrpages--;
    lru_del(pg);
    if (page_uflag_test(pg, PGU_DIRTY)) {
        page_uflag_clear(pg, PGU_DIRTY);
        __atomic_fetch_sub(&pagecache_dirty, 1, __ATOMIC_RELAXED);
        uint64_t f = arch_irq_save();
        spin_lock_ipi(&dirty_lock);
        if (m->nrdirty) m->nrdirty--;
        spin_unlock(&dirty_lock);
        arch_irq_restore(f);
    }
    page_uflag_clear(pg, PGU_CACHE);
    pg->mapping = nullptr;
    __atomic_fetch_sub(&pagecache_pages, 1, __ATOMIC_RELAXED);
    page_put(pg);
}

void mapping_truncate(struct address_space *m, uint64_t from) {
    struct page *batch[32];
    for (;;) {
        uint64_t f = m_lock(m);
        int n = rt_gang(m, from, UINT64_MAX, batch, 32);
        m_unlock(m, f);
        if (!n) break;
        for (int i = 0; i < n; i++) {
            struct page *pg = batch[i];
            filemap_wait_page(pg);
            f = m_lock(m);
            if (pg->mapping == m && rt_lookup(m, pg->index) == pg) {
                if (pg->mapcount) rmap_unmap_file_page(pg);   /* best effort: mapped pages become anonymous */
                cache_remove_locked(m, pg);
            }
            m_unlock(m, f);
            page_put(pg);
        }
    }
    if (!from) {
        uint64_t f = arch_irq_save();
        spin_lock_ipi(&dirty_lock);
        if (m->on_dirty_list) { list_del(&m->dirty_node); m->on_dirty_list = false; }
        m->nrdirty = 0;
        spin_unlock(&dirty_lock);
        arch_irq_restore(f);
    }
}

/* ------------------------------------------------------------------ reading */
void filemap_read_done(struct page *pg, int err) {
    if (err) page_uflag_set(pg, PGU_ERROR);
    else { page_uflag_clear(pg, PGU_ERROR); page_uflag_set(pg, PGU_UPTODATE); }
    __atomic_fetch_and(&pg->uflags, (uint16_t)~PGU_LOCKED, __ATOMIC_RELEASE);
    wake_up(&page_wq);
    if (!err) lru_add(pg);
}

void filemap_wait_page(struct page *pg) {
    /* uninterruptible: a pending signal must not turn this into a busy loop */
    while (__atomic_load_n(&pg->uflags, __ATOMIC_ACQUIRE) & PGU_LOCKED) {
        uint64_t f = sched_wait_lock();
        if (!(__atomic_load_n(&pg->uflags, __ATOMIC_ACQUIRE) & PGU_LOCKED)) { sched_wait_unlock(f); break; }
        wait_event_uninterruptible_locked(&page_wq, f);
    }
}

static bool trylock_page(struct page *pg) {
    return !(__atomic_fetch_or(&pg->uflags, PGU_LOCKED, __ATOMIC_ACQ_REL) & PGU_LOCKED);
}
static void unlock_page(struct page *pg) {
    __atomic_fetch_and(&pg->uflags, (uint16_t)~PGU_LOCKED, __ATOMIC_RELEASE);
    wake_up(&page_wq);
}
static void lock_page(struct page *pg) {
    while (!trylock_page(pg)) filemap_wait_page(pg);
}

static int start_read(struct address_space *m, struct page *pg) {
    __atomic_fetch_add(&pagecache_reads, 1, __ATOMIC_RELAXED);
    int e = m->ops && m->ops->readpage ? m->ops->readpage(m, pg) : 0;
    if (e || !m->ops || !m->ops->readpage) filemap_read_done(pg, e);
    return e;
}

struct page *filemap_find(struct address_space *m, uint64_t idx) {
    uint64_t f = m_lock(m);
    struct page *pg = rt_lookup(m, idx);
    if (pg) page_ref_inc(pg);
    m_unlock(m, f);
    return pg;
}

/* new locked page in the cache, or the existing one (referenced either way) */
static struct page *add_page(struct address_space *m, uint64_t idx, bool *fresh, bool locked) {
    *fresh = false;
    for (int tries = 0;; tries++) {
        struct page *pg = filemap_find(m, idx);
        if (pg) return pg;
        paddr_t pa = pmm_alloc_pages(0);
        if (!pa) {
            if (tries < 4 && (pagecache_reclaim(32) || tries < 2)) { writeback_kick(); continue; }
            return nullptr;
        }
        pg = phys_to_page(pa);
        pg->mapping = m;
        pg->index = (uint32_t)idx;
        pg->private = 0;
        page_uflag_set(pg, PGU_CACHE | PGU_REFERENCED | (locked ? PGU_LOCKED : 0));
        uint64_t f = m_lock(m);
        int r = rt_lookup(m, idx) ? -EEXIST : rt_insert(m, idx, pg);
        if (!r) { m->nrpages++; page_ref_inc(pg); }
        m_unlock(m, f);
        if (!r) { __atomic_fetch_add(&pagecache_pages, 1, __ATOMIC_RELAXED); *fresh = true; return pg; }
        pg->uflags = 0; pg->mapping = nullptr;
        page_put(pg);
        if (r != -EEXIST) return nullptr;
    }
}

int filemap_get_page(struct address_space *m, uint64_t idx, bool create, struct page **out) {
    bool fresh;
    bool need_read = !create && m->ops && m->ops->readpage;
    struct page *pg = add_page(m, idx, &fresh, true);
    if (!pg) return -ENOMEM;
    if (fresh) {
        if (need_read) start_read(m, pg);
        else {
            memset(PHYS_TO_VIRT(page_to_phys(pg)), 0, PAGE_SIZE);
            page_uflag_set(pg, PGU_UPTODATE);
            unlock_page(pg);
            lru_add(pg);
        }
    } else {
        __atomic_fetch_add(&pagecache_hits, 1, __ATOMIC_RELAXED);
        pagecache_mark_referenced(pg);
    }
    while (!page_uflag_test(pg, PGU_UPTODATE)) {
        filemap_wait_page(pg);
        if (page_uflag_test(pg, PGU_UPTODATE)) break;
        if (page_uflag_test(pg, PGU_ERROR) || pg->mapping != m) { page_put(pg); return -EIO; }
        if (trylock_page(pg)) {          /* left unread (e.g. created by a failed fault) */
            if (page_uflag_test(pg, PGU_UPTODATE)) { unlock_page(pg); break; }
            if (create) {
                memset(PHYS_TO_VIRT(page_to_phys(pg)), 0, PAGE_SIZE);
                filemap_read_done(pg, 0);
            } else start_read(m, pg);
        }
    }
    *out = pg;
    return 0;
}

/* start reads of up to n uncached pages after idx (no waiting) */
void filemap_readahead(struct address_space *m, uint64_t idx, unsigned n) {
    if (!m->ops || !m->ops->readpage) return;
    for (unsigned i = 0; i < n; i++) {
        bool fresh;
        struct page *pg = filemap_find(m, idx + i);
        if (pg) { page_put(pg); continue; }
        pg = add_page(m, idx + i, &fresh, true);
        if (!pg) return;
        if (fresh) start_read(m, pg);
        page_put(pg);
    }
}

int filemap_fault(struct address_space *m, uint64_t idx, uint64_t size, bool shared, paddr_t *pa) {
    if (idx >= (size + PAGE_SIZE - 1) / PAGE_SIZE) return -ENXIO;
    uint64_t f = m_lock(m);
    struct page *pg = rt_lookup(m, idx);
    int r = 0;
    if (pg && page_uflag_test(pg, PGU_UPTODATE)) {
        page_ref_inc(pg);
        pagecache_mark_referenced(pg);
        *pa = page_to_phys(pg);
    } else if (pg || !m->atomic_fill) {
        r = -EAGAIN;
    } else {
        paddr_t np = pmm_alloc_pages(0);
        if (!np) r = -ENOMEM;
        else {
            pg = phys_to_page(np);
            pg->mapping = m; pg->index = (uint32_t)idx; pg->private = 0;
            page_uflag_set(pg, PGU_CACHE | PGU_REFERENCED | PGU_LOCKED);
            if (rt_insert(m, idx, pg)) { pg->uflags = 0; pg->mapping = nullptr; page_put(pg); r = -ENOMEM; }
            else {
                m->nrpages++;
                __atomic_fetch_add(&pagecache_pages, 1, __ATOMIC_RELAXED);
                __atomic_fetch_add(&pagecache_filled, 1, __ATOMIC_RELAXED);
                start_read(m, pg);          /* atomic: completes before returning */
                page_ref_inc(pg);
                *pa = np;
            }
        }
    }
    m_unlock(m, f);
    return r;
}

int filemap_fault_prepare(struct address_space *m, uint64_t idx) {
    struct page *pg;
    int r = filemap_get_page(m, idx, false, &pg);
    if (!r) page_put(pg);
    return r;
}

/* ------------------------------------------------------------------ generic file I/O */
ssize_t filemap_read(struct address_space *m, uint64_t size, void *buf, size_t n, uint64_t off) {
    if (off >= size) return 0;
    n = MIN(n, size - off);
    size_t done = 0;
    int err = 0;
    uint64_t ra_next = 0;
    while (done < n) {
        uint64_t pos = off + done;
        uint64_t idx = pos / PAGE_SIZE;
        size_t po = pos % PAGE_SIZE, chunk = MIN(n - done, PAGE_SIZE - po);
        struct page *pg = filemap_find(m, idx);
        if (!pg || !page_uflag_test(pg, PGU_UPTODATE)) {
            if (pg) page_put(pg);
            uint64_t last = (size - 1) / PAGE_SIZE;
            if (!m->atomic_fill && idx + 1 <= last && idx + 1 >= ra_next) {   /* sequential: read ahead the rest of the request + 16 */
                unsigned ra = (unsigned)MIN(last - idx, (uint64_t)((n - done) / PAGE_SIZE + 16));
                filemap_readahead(m, idx + 1, ra);
                ra_next = idx + 1 + ra;
            }
            if ((err = filemap_get_page(m, idx, false, &pg))) break;
        } else {
            __atomic_fetch_add(&pagecache_hits, 1, __ATOMIC_RELAXED);
            pagecache_mark_referenced(pg);
        }
        err = pc_copy_out((uint8_t *)buf + done, (uint8_t *)PHYS_TO_VIRT(page_to_phys(pg)) + po, chunk);
        page_put(pg);
        if (err) { err = -EFAULT; break; }
        done += chunk;
    }
    return done ? (ssize_t)done : err;
}

ssize_t filemap_write(struct address_space *m, uint64_t *size, const void *buf, size_t n, uint64_t off) {
    size_t done = 0;
    int err = 0;
    while (done < n) {
        uint64_t pos = off + done;
        uint64_t idx = pos / PAGE_SIZE;
        size_t po = pos % PAGE_SIZE, chunk = MIN(n - done, PAGE_SIZE - po);
        /* no read needed for whole-page overwrites and pages entirely past the old end */
        bool whole = po == 0 && (chunk == PAGE_SIZE || pos + chunk >= *size);
        bool create = whole || idx * PAGE_SIZE >= *size;
        struct page *pg;
        if ((err = filemap_get_page(m, idx, create, &pg))) break;
        /* blocks are allocated with the (uptodate, referenced) page in hand: holes were read
         * as zeroes, so newly allocated blocks never expose stale disk contents */
        if (m->ops && m->ops->prepare_write && (err = m->ops->prepare_write(m, idx, po, po + chunk))) { page_put(pg); break; }
        pagecache_mark_dirty(pg);
        uint8_t *va = (uint8_t *)PHYS_TO_VIRT(page_to_phys(pg));
        err = pc_copy_in(va + po, (const uint8_t *)buf + done, chunk);
        pagecache_mark_dirty(pg);         /* writeback may have cleaned it during the copy */
        page_put(pg);
        if (err) { err = -EFAULT; break; }
        done += chunk;
        if (pos + chunk > *size) *size = pos + chunk;
    }
    return done ? (ssize_t)done : err;
}

void filemap_zero_tail(struct address_space *m, uint64_t size) {
    if (!(size % PAGE_SIZE)) return;
    struct page *pg = filemap_find(m, size / PAGE_SIZE);
    if (!pg) return;
    if (page_uflag_test(pg, PGU_UPTODATE)) {
        memset((uint8_t *)PHYS_TO_VIRT(page_to_phys(pg)) + size % PAGE_SIZE, 0, PAGE_SIZE - size % PAGE_SIZE);
        pagecache_mark_dirty(pg);
    }
    page_put(pg);
}

/* ------------------------------------------------------------------ write-back */
int filemap_writeback(struct address_space *m, uint64_t start, uint64_t end) {
    if (!m->ops || !m->ops->writepage) return 0;
    struct page *batch[32];
    int err = 0;
    uint64_t next = start;
    for (;;) {
        uint64_t f = m_lock(m);
        int n = rt_gang(m, next, end, batch, 32);
        m_unlock(m, f);
        if (!n) break;
        next = batch[n - 1]->index + 1ull;
        for (int i = 0; i < n; i++) {
            struct page *pg = batch[i];
            if (page_uflag_test(pg, PGU_DIRTY) && pg->mapping == m) {
                lock_page(pg);
                if (pg->mapping == m && page_uflag_test(pg, PGU_UPTODATE) && clear_dirty(m, pg)) {
                    if (pg->mapcount) rmap_mkclean_file_page(pg);   /* re-dirtied by the next write fault */
                    int e = m->ops->writepage(m, pg);
                    if (e) { err = e; m->wb_errors++; pagecache_mark_dirty(pg); }
                    else __atomic_fetch_add(&pagecache_written, 1, __ATOMIC_RELAXED);
                }
                unlock_page(pg);
                lru_add(pg);
            }
            page_put(pg);
        }
        if (next == 0) break;
    }
    uint64_t f = arch_irq_save();
    spin_lock_ipi(&dirty_lock);
    if (!m->nrdirty && m->on_dirty_list) { list_del(&m->dirty_node); m->on_dirty_list = false; }
    spin_unlock(&dirty_lock);
    arch_irq_restore(f);
    return err;
}

/* write back every dirty mapping once; the host inode is pinned while its pages are written */
int writeback_all(void) {
    int err = 0;
    for (int pass = 0; pass < 256; pass++) {
        uint64_t f = arch_irq_save();
        spin_lock_ipi(&dirty_lock);
        struct address_space *m = nullptr;
        list_for_each(it, &dirty_list) {
            struct address_space *c = list_entry(it, struct address_space, dirty_node);
            if (c->host) { m = c; break; }
        }
        if (m) { list_del(&m->dirty_node); list_add_tail(&dirty_list, &m->dirty_node); iget(m->host); }
        spin_unlock(&dirty_lock);
        arch_irq_restore(f);
        if (!m) break;
        int e = filemap_writeback(m, 0, UINT64_MAX);
        if (e) err = e;
        iput(m->host);
        f = arch_irq_save();
        spin_lock_ipi(&dirty_lock);
        bool empty = list_empty(&dirty_list);
        spin_unlock(&dirty_lock);
        arch_irq_restore(f);
        if (empty) break;
    }
    return err;
}

/* inode cache: may the host of m (refcount *ref, icache lock held) be evicted now? discard:
 * it was unlinked, dirty pages are dropped. On success the mapping never rejoins the dirty
 * list, so writeback_all() cannot pick (and iget) the dying inode any more. */
bool mapping_detach(struct address_space *m, int *ref, bool discard) {
    uint64_t f = arch_irq_save();
    spin_lock_ipi(&dirty_lock);
    bool ok = __atomic_load_n(ref, __ATOMIC_ACQUIRE) == 0 && (discard || (!m->on_dirty_list && !m->nrdirty));
    if (ok) {
        if (m->on_dirty_list) { list_del(&m->dirty_node); m->on_dirty_list = false; }
        m->no_writeback = true;
    }
    spin_unlock(&dirty_lock);
    arch_irq_restore(f);
    return ok;
}

void writeback_kick(void) {
    __atomic_store_n(&wb_kicked, 1, __ATOMIC_RELEASE);
    wake_up(&wb_wq);
}

/* "writeback": dirty data older than ~5 s (or on memory pressure / too many dirty pages) */
uint64_t writeback_runs;
static void wb_thread(void *arg) {
    for (;;) {
        uint64_t f = sched_wait_lock();
        if (!__atomic_load_n(&wb_kicked, __ATOMIC_ACQUIRE)) wait_event_timeout_locked(&wb_wq, 5000000000ull, f);
        else sched_wait_unlock(f);
        __atomic_store_n(&wb_kicked, 0, __ATOMIC_RELEASE);
        if (!__atomic_load_n(&pagecache_dirty, __ATOMIC_RELAXED)) continue;
        writeback_runs++;
        writeback_all();
    }
}

void writeback_init(void) { thread_create("writeback", wb_thread, nullptr); }

/* ------------------------------------------------------------------ reclaim */
/*
 * Drop up to want clean pages from the LRU (second chance: referenced pages are rotated once),
 * unmapping them from every address space through the reverse map. Never sleeps: the mapping
 * lock and mm locks are only trylocked (the LRU lock nests inside the mapping lock), so it can
 * run from allocation paths with arbitrary locks held.
 */
static int reclaim_busy;
uint64_t pagecache_reclaim(uint64_t want) {
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
        struct address_space *m = pg->mapping;
        /* while pg is on the LRU its mapping cannot go away without m->lock */
        bool locked = m && spin_trylock(&m->lock);
        spin_unlock(&lru_lock);
        if (!locked) { arch_irq_restore(irq); continue; }
        __atomic_fetch_add(&vm_stats.reclaim_scanned, 1, __ATOMIC_RELAXED);
        bool ok = pg->mapping == m && rt_lookup(m, pg->index) == pg &&
                  !page_uflag_test(pg, PGU_DIRTY | PGU_LOCKED) && page_uflag_test(pg, PGU_LRU) &&
                  rmap_unmap_file_page(pg) && page_ref_read(pg) == 1;
        if (ok) {
            cache_remove_locked(m, pg);           /* frees it */
            freed++;
            __atomic_fetch_add(&pagecache_reclaimed, 1, __ATOMIC_RELAXED);
        }
        spin_unlock(&m->lock);
        arch_irq_restore(irq);
    }
    __atomic_store_n(&reclaim_busy, 0, __ATOMIC_RELEASE);
    return freed;
}
