/* Binary buddy allocator over the Limine memory map. */
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/spinlock.h>
#include <kernel/sched.h>

struct page *page_array;
uint64_t max_pfn;

static struct list_node free_lists[MAX_ORDER];
static uint64_t free_count[MAX_ORDER];
static uint64_t total_pages, free_pages;
static spinlock_t buddy_lock = SPINLOCK_INIT;

#define PCP_HIGH 16             /* at most 64 KiB per CPU, 2 MiB at MAX_CPUS */
struct page_cpu_cache {
    spinlock_t lock;
    struct list_node pages;
    unsigned nr;
    uint64_t alloc_hits, free_hits, drained_pages;
};
static struct page_cpu_cache pcaches[MAX_CPUS];
static bool pcaches_enabled;
static uint64_t cached_pages;
static spinlock_t cache_drain_lock = SPINLOCK_INIT;
void slab_reclaim_cpu_caches(void) __attribute__((weak));

/* Before SMP setup there may be no valid per-CPU pointer. Once enabled, lock
 * waiters must service TLB IPIs (callers may already hold an mm/page-table lock). */
static uint64_t buddy_lock_irqsave(void) {
    uint64_t f = arch_irq_save();
    if (__atomic_load_n(&pcaches_enabled, __ATOMIC_ACQUIRE)) spin_lock_ipi(&buddy_lock);
    else spin_lock(&buddy_lock);
    return f;
}

static void free_block(uint64_t pfn, unsigned order) {
    /* coalesce with buddies */
    while (order < MAX_ORDER - 1) {
        uint64_t buddy = pfn ^ (1ULL << order);
        if (buddy >= max_pfn) break;
        struct page *b = &page_array[buddy];
        /* A local cache may mark an allocated buddy PG_PCPU without buddy_lock.
         * Both transitions are non-free; use an atomic load for that flag race. */
        if (!(__atomic_load_n(&b->flags, __ATOMIC_RELAXED) & PG_FREE) || b->order != order) break;
        list_del(&b->node);
        b->flags &= ~PG_FREE;
        free_count[order]--;
        pfn &= ~(1ULL << order);
        order++;
    }
    struct page *p = &page_array[pfn];
    p->flags = PG_FREE;
    p->order = order;
    list_add(&free_lists[order], &p->node);
    free_count[order]++;
}

/* buddy_lock held. Cache transfers use the same helper, but never double-charge
 * free_pages: it includes both buddy pages and cached order-0 pages. */
static struct page *alloc_block_locked(unsigned order) {
    unsigned o = order;
    while (o < MAX_ORDER && list_empty(&free_lists[o])) o++;
    if (o == MAX_ORDER) return nullptr;
    struct page *p = list_first(&free_lists[o], struct page, node);
    list_del(&p->node);
    free_count[o]--;
    uint64_t pfn = p - page_array;
    while (o > order) {         /* split, returning upper halves to the free lists */
        o--;
        struct page *half = &page_array[pfn + (1ULL << o)];
        half->flags = PG_FREE;
        half->order = o;
        list_add(&free_lists[o], &half->node);
        free_count[o]++;
    }
    p->flags = 0;
    p->order = order;
    p->refcount = 1;
    p->slab = nullptr;
    __atomic_fetch_sub(&free_pages, 1ULL << order, __ATOMIC_RELAXED);
    return p;
}

struct page *page_alloc(unsigned order) {
    if (order >= MAX_ORDER) return nullptr;
    bool enabled = __atomic_load_n(&pcaches_enabled, __ATOMIC_ACQUIRE);
    if (!order && enabled) {
        uint64_t f = arch_irq_save();             /* select CPU only after disabling migration */
        struct page_cpu_cache *c = &pcaches[this_cpu()->id];
        spin_lock_ipi(&c->lock);
        if (c->nr) {
            struct page *p = list_first(&c->pages, struct page, node);
            list_del(&p->node);
            c->nr--;
            assert(p->flags == PG_PCPU && !p->order);
            __atomic_store_n(&p->flags, 0, __ATOMIC_RELAXED);
            p->refcount = 1;
            p->slab = nullptr;
            __atomic_fetch_sub(&cached_pages, 1, __ATOMIC_RELAXED);
            __atomic_fetch_sub(&free_pages, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&c->alloc_hits, 1, __ATOMIC_RELAXED);
            spin_unlock_irqrestore(&c->lock, f);
            return p;
        }
        spin_unlock_irqrestore(&c->lock, f);
    }
    uint64_t f = buddy_lock_irqsave();
    struct page *p = alloc_block_locked(order);
    spin_unlock_irqrestore(&buddy_lock, f);
    if (p || !enabled) return p;
    /* Cached pages must not cause false OOM or prevent a contiguous allocation.
     * No cache/buddy lock is held here. Drain remote caches too, then retry once. */
    pmm_drain_cpu_caches();
    f = buddy_lock_irqsave();
    p = alloc_block_locked(order);
    spin_unlock_irqrestore(&buddy_lock, f);
    if (!p && slab_reclaim_cpu_caches) {
        slab_reclaim_cpu_caches();
        pmm_drain_cpu_caches();
        f = buddy_lock_irqsave();
        p = alloc_block_locked(order);
        spin_unlock_irqrestore(&buddy_lock, f);
    }
    return p;
}

void page_free(struct page *p, unsigned order) {
    assert(order < MAX_ORDER);
    if (!order && __atomic_load_n(&pcaches_enabled, __ATOMIC_ACQUIRE)) {
        uint64_t f = arch_irq_save();
        struct page_cpu_cache *c = &pcaches[this_cpu()->id];
        spin_lock_ipi(&c->lock);
        assert(!(p->flags & (PG_FREE | PG_RESERVED | PG_PCPU)));
        if (c->nr < PCP_HIGH) {
            assert(!p->order);
            __atomic_store_n(&p->flags, PG_PCPU, __ATOMIC_RELAXED);
            p->refcount = 0;
            p->slab = nullptr;
            list_add(&c->pages, &p->node);
            c->nr++;
            __atomic_fetch_add(&cached_pages, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&free_pages, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&c->free_hits, 1, __ATOMIC_RELAXED);
            spin_unlock_irqrestore(&c->lock, f);
            return;
        }
        spin_unlock_irqrestore(&c->lock, f);
    }
    uint64_t f = buddy_lock_irqsave();
    assert(!(p->flags & (PG_FREE | PG_RESERVED | PG_PCPU)));
    __atomic_fetch_add(&free_pages, 1ULL << order, __ATOMIC_RELAXED);
    free_block(p - page_array, order);
    spin_unlock_irqrestore(&buddy_lock, f);
}

void pmm_enable_cpu_caches(void) {
    assert(!__atomic_load_n(&pcaches_enabled, __ATOMIC_RELAXED));
    for (int i = 0; i < MAX_CPUS; i++) list_init(&pcaches[i].pages);
    __atomic_store_n(&pcaches_enabled, true, __ATOMIC_RELEASE);
    pr_info("pmm: per-CPU order-0 caches enabled (%d pages/CPU)\n", PCP_HIGH);
}

/* Lock order is deliberately flat: detach under the cache lock, release it, then
 * coalesce under buddy_lock. A drain-only lock serializes detach/transfer sequences,
 * so an OOM retry cannot overlook pages detached by an unfinished concurrent drain.
 * Never hold a cache/buddy lock while acquiring another cache lock.
 * Detached pages remain PG_PCPU until safely transferred; stats stay approximate
 * during a transfer, while free_pages is unchanged throughout it. */
void pmm_drain_cpu_caches(void) {
    if (!__atomic_load_n(&pcaches_enabled, __ATOMIC_ACQUIRE)) return;
    uint64_t f = arch_irq_save();
    spin_lock_ipi(&cache_drain_lock);
    for (int i = 0; i < MAX_CPUS; i++) {
        struct page_cpu_cache *c = &pcaches[i];
        struct list_node detached = LIST_INIT(detached);
        spin_lock_ipi(&c->lock);
        unsigned nr = c->nr;
        while (!list_empty(&c->pages)) {
            struct list_node *node = c->pages.next;
            list_del(node);
            list_add(&detached, node);
        }
        c->nr = 0;
        __atomic_fetch_sub(&cached_pages, nr, __ATOMIC_RELAXED);
        __atomic_fetch_add(&c->drained_pages, nr, __ATOMIC_RELAXED);
        spin_unlock(&c->lock);
        if (!nr) continue;
        spin_lock_ipi(&buddy_lock);
        list_for_each_safe(it, tmp, &detached) {
            struct page *p = list_entry(it, struct page, node);
            list_del(it);
            assert(p->flags == PG_PCPU);
            p->flags = 0;
            free_block(p - page_array, 0);
        }
        spin_unlock(&buddy_lock);
    }
    spin_unlock(&cache_drain_lock);
    arch_irq_restore(f);
}

void pmm_cache_stats(struct pmm_cache_stats *stats) {
    memset(stats, 0, sizeof *stats);
    stats->cached_pages = __atomic_load_n(&cached_pages, __ATOMIC_RELAXED);
    for (int i = 0; i < MAX_CPUS; i++) {
        stats->alloc_hits += __atomic_load_n(&pcaches[i].alloc_hits, __ATOMIC_RELAXED);
        stats->free_hits += __atomic_load_n(&pcaches[i].free_hits, __ATOMIC_RELAXED);
        stats->drained_pages += __atomic_load_n(&pcaches[i].drained_pages, __ATOMIC_RELAXED);
    }
}

paddr_t pmm_alloc_pages(unsigned order) {
    struct page *p = page_alloc(order);
    return p ? page_to_phys(p) : 0;
}
paddr_t pmm_alloc_zeroed(unsigned order) {
    paddr_t pa = pmm_alloc_pages(order);
    if (pa) memset(PHYS_TO_VIRT(pa), 0, PAGE_SIZE << order);
    return pa;
}
void pmm_free_pages(paddr_t pa, unsigned order) { page_free(phys_to_page(pa), order); }

void pmm_stats(uint64_t *fp, uint64_t *tp) { *fp = __atomic_load_n(&free_pages, __ATOMIC_RELAXED); *tp = total_pages; }

/* Add [base, base+len) to the allocator using maximal aligned blocks. */
static void add_range(paddr_t base, uint64_t len) {
    uint64_t pfn = ALIGN_UP(base, PAGE_SIZE) >> PAGE_SHIFT;
    uint64_t end = ALIGN_DOWN(base + len, PAGE_SIZE) >> PAGE_SHIFT;
    while (pfn < end) {
        unsigned order = MAX_ORDER - 1;
        while (order && ((pfn & ((1ULL << order) - 1)) || pfn + (1ULL << order) > end)) order--;
        for (uint64_t i = 0; i < (1ULL << order); i++) page_array[pfn + i].flags = 0;
        free_block(pfn, order);
        total_pages += 1ULL << order;
        free_pages += 1ULL << order;
        pfn += 1ULL << order;
    }
}

void pmm_init(void) {
    struct limine_memmap_response *mm = boot_memmap();
    for (unsigned i = 0; i < MAX_ORDER; i++) list_init(&free_lists[i]);

    uint64_t top = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE || e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
            e->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES || e->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE)
            top = MAX(top, e->base + e->length);
    }
    max_pfn = top >> PAGE_SHIFT;
    uint64_t array_bytes = ALIGN_UP(max_pfn * sizeof(struct page), PAGE_SIZE);

    /* carve the page array out of the first usable region big enough */
    paddr_t array_pa = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= array_bytes) {
            array_pa = e->base;
            break;
        }
    }
    if (!array_pa) panic("pmm: no room for page array (%lu bytes)", array_bytes);
    page_array = PHYS_TO_VIRT(array_pa);
    for (uint64_t i = 0; i < max_pfn; i++)
        page_array[i] = (struct page){ .flags = PG_RESERVED };

    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        paddr_t base = e->base;
        uint64_t len = e->length;
        if (base == array_pa) { base += array_bytes; len -= array_bytes; }
        if (base < 0x1000) { uint64_t d = 0x1000 - base; if (len <= d) continue; base += d; len -= d; }
        add_range(base, len);
    }
    pr_info("pmm: buddy allocator ready, %lu MiB free (%lu pages, page array %lu KiB)\n",
            (free_pages * PAGE_SIZE) >> 20, free_pages, array_bytes >> 10);
}

void pmm_selftest(void) {
    uint64_t before = free_pages;
    paddr_t a[64];
    for (int i = 0; i < 64; i++) {
        a[i] = pmm_alloc_pages(i % 5);
        assert(a[i] && (a[i] & ((PAGE_SIZE << (i % 5)) - 1)) == 0);
        memset(PHYS_TO_VIRT(a[i]), 0xAB, PAGE_SIZE << (i % 5));
    }
    for (int i = 0; i < 64; i += 2) pmm_free_pages(a[i], i % 5);
    for (int i = 1; i < 64; i += 2) pmm_free_pages(a[i], i % 5);
    assert(free_pages == before);
    pr_info("pmm: self-test passed\n");
}

void pmm_cache_selftest(void) {
    uint64_t f = arch_irq_save(), before = __atomic_load_n(&free_pages, __ATOMIC_RELAXED);
    struct page *p = page_alloc(0);
    assert(p);
    page_free(p, 0);
    struct page *again = page_alloc(0);
    assert(again == p && again->flags == 0 && again->refcount == 1 && !again->slab);
    page_free(again, 0);
    struct page *pages[PCP_HIGH * 2];
    for (unsigned i = 0; i < ARRAY_SIZE(pages); i++) { pages[i] = page_alloc(0); assert(pages[i]); }
    for (unsigned i = 0; i < ARRAY_SIZE(pages); i++) page_free(pages[i], 0);
    pmm_drain_cpu_caches();
    assert(__atomic_load_n(&free_pages, __ATOMIC_RELAXED) == before);
    arch_irq_restore(f);
    pr_info("pmm: per-CPU cache self-test passed\n");
}
