/* Binary buddy allocator over the Limine memory map. */
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/spinlock.h>

struct page *page_array;
uint64_t max_pfn;

static struct list_node free_lists[MAX_ORDER];
static uint64_t free_count[MAX_ORDER];
static uint64_t total_pages, free_pages;
static spinlock_t buddy_lock = SPINLOCK_INIT;

static void free_block(uint64_t pfn, unsigned order) {
    /* coalesce with buddies */
    while (order < MAX_ORDER - 1) {
        uint64_t buddy = pfn ^ (1ULL << order);
        if (buddy >= max_pfn) break;
        struct page *b = &page_array[buddy];
        if (!(b->flags & PG_FREE) || b->order != order) break;
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

struct page *page_alloc(unsigned order) {
    if (order >= MAX_ORDER) return nullptr;
    uint64_t f = spin_lock_irqsave(&buddy_lock);
    unsigned o = order;
    while (o < MAX_ORDER && list_empty(&free_lists[o])) o++;
    if (o == MAX_ORDER) { spin_unlock_irqrestore(&buddy_lock, f); return nullptr; }
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
    free_pages -= 1ULL << order;
    spin_unlock_irqrestore(&buddy_lock, f);
    return p;
}

void page_free(struct page *p, unsigned order) {
    uint64_t f = spin_lock_irqsave(&buddy_lock);
    assert(!(p->flags & (PG_FREE | PG_RESERVED)));
    free_pages += 1ULL << order;
    free_block(p - page_array, order);
    spin_unlock_irqrestore(&buddy_lock, f);
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

void pmm_stats(uint64_t *fp, uint64_t *tp) { *fp = free_pages; *tp = total_pages; }

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
