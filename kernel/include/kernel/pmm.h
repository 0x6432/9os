#pragma once
/* Physical memory manager: binary buddy allocator. */
#include <kernel/types.h>
#include <kernel/list.h>

#define MAX_ORDER 11          /* orders 0..10 => 4 KiB .. 4 MiB */

enum {
    PG_RESERVED = 1 << 0,
    PG_FREE     = 1 << 1,     /* head of a free buddy block */
    PG_SLAB     = 1 << 2,
    PG_LARGE    = 1 << 3,     /* head of a large kmalloc block */
};

struct kmem_slab;
struct page {
    struct list_node node;
    uint8_t order;
    uint8_t flags;
    uint16_t _pad;
    int32_t refcount;
    struct kmem_slab *slab;
};

extern struct page *page_array;
extern uint64_t max_pfn;

void pmm_init(void);
struct page *page_alloc(unsigned order);
void page_free(struct page *pg, unsigned order);
paddr_t pmm_alloc_pages(unsigned order);   /* returns 0 on failure */
paddr_t pmm_alloc_zeroed(unsigned order);
void pmm_free_pages(paddr_t pa, unsigned order);
void pmm_stats(uint64_t *free_pages, uint64_t *total_pages);
void pmm_selftest(void);

static inline paddr_t page_to_phys(struct page *p) { return (paddr_t)(p - page_array) << PAGE_SHIFT; }
static inline struct page *phys_to_page(paddr_t pa) { return &page_array[pa >> PAGE_SHIFT]; }
/* drop one reference on an order-0 page (shared by tmpfs and user mappings) */
static inline void page_ref_inc(struct page *pg) { __atomic_fetch_add(&pg->refcount, 1, __ATOMIC_RELAXED); }
static inline bool page_ref_dec_test(struct page *pg) { return __atomic_sub_fetch(&pg->refcount, 1, __ATOMIC_ACQ_REL) <= 0; }
static inline int page_ref_read(struct page *pg) { return __atomic_load_n(&pg->refcount, __ATOMIC_RELAXED); }
static inline void page_put(struct page *pg) { if (page_ref_dec_test(pg)) page_free(pg, 0); }
static inline void page_put_pa(paddr_t pa) { page_put(phys_to_page(pa)); }
static inline unsigned size_to_order(size_t size) {
    unsigned o = 0;
    while ((PAGE_SIZE << o) < size) o++;
    return o;
}
