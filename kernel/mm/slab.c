/* Slab allocator and kmalloc front-end. */
#include <kernel/slab.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/kmalloc.h>
#include <kernel/printk.h>
#include <kernel/string.h>

#define KMALLOC_MIN_SHIFT 4     /* 16 bytes */
#define KMALLOC_MAX_SHIFT 11    /* 2048 bytes */
static struct kmem_cache kmalloc_caches[KMALLOC_MAX_SHIFT - KMALLOC_MIN_SHIFT + 1];
static const char *kmalloc_names[] = { "kmalloc-16", "kmalloc-32", "kmalloc-64", "kmalloc-128",
                                       "kmalloc-256", "kmalloc-512", "kmalloc-1024", "kmalloc-2048" };
static struct kmem_cache cache_cache;   /* cache of kmem_cache structs */

void kmem_cache_init(struct kmem_cache *c, const char *name, size_t size, size_t align) {
    if (align < sizeof(void *)) align = sizeof(void *);
    memset(c, 0, sizeof *c);
    c->name = name;
    c->obj_size = ALIGN_UP(MAX(size, sizeof(void *)), align);
    c->first_off = ALIGN_UP(sizeof(struct kmem_slab), align);
    /* choose a slab order so that at least 8 objects fit and waste is small */
    unsigned order = 0;
    while (order < 4 && ((PAGE_SIZE << order) - c->first_off) / c->obj_size < 8) order++;
    c->slab_order = order;
    c->objs_per_slab = ((PAGE_SIZE << order) - c->first_off) / c->obj_size;
    list_init(&c->partial); list_init(&c->full); list_init(&c->empty);
}

struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align) {
    struct kmem_cache *c = kmem_cache_alloc(&cache_cache);
    if (c) kmem_cache_init(c, name, size, align);
    return c;
}

static struct kmem_slab *slab_grow(struct kmem_cache *c) {
    struct page *pg = page_alloc(c->slab_order);
    if (!pg) return nullptr;
    uint8_t *base = PHYS_TO_VIRT(page_to_phys(pg));
    struct kmem_slab *s = (struct kmem_slab *)base;
    s->cache = c;
    s->inuse = 0;
    s->freelist = nullptr;
    for (int i = c->objs_per_slab - 1; i >= 0; i--) {
        void **obj = (void **)(base + c->first_off + i * c->obj_size);
        *obj = s->freelist;
        s->freelist = obj;
    }
    for (unsigned i = 0; i < (1u << c->slab_order); i++) {
        pg[i].flags |= PG_SLAB;
        pg[i].slab = s;
    }
    c->nr_slabs++;
    return s;
}

void *kmem_cache_alloc(struct kmem_cache *c) {
    uint64_t f = spin_lock_irqsave(&c->lock);
    struct kmem_slab *s;
    if (!list_empty(&c->partial)) s = list_first(&c->partial, struct kmem_slab, node);
    else if (!list_empty(&c->empty)) {
        s = list_first(&c->empty, struct kmem_slab, node);
        list_del(&s->node);
        list_add(&c->partial, &s->node);
    } else {
        s = slab_grow(c);
        if (!s) { spin_unlock_irqrestore(&c->lock, f); return nullptr; }
        list_add(&c->partial, &s->node);
    }
    void **obj = s->freelist;
    s->freelist = *obj;
    s->inuse++;
    c->nr_active++;
    if (s->inuse == c->objs_per_slab) { list_del(&s->node); list_add(&c->full, &s->node); }
    spin_unlock_irqrestore(&c->lock, f);
    return obj;
}

void kmem_cache_free(struct kmem_cache *c, void *obj) {
    struct page *pg = phys_to_page(VIRT_TO_PHYS(obj));
    struct kmem_slab *s = pg->slab;
    assert((pg->flags & PG_SLAB) && s->cache == c);
    uint64_t f = spin_lock_irqsave(&c->lock);
    bool was_full = s->inuse == c->objs_per_slab;
    *(void **)obj = s->freelist;
    s->freelist = obj;
    s->inuse--;
    c->nr_active--;
    if (s->inuse == 0) {
        list_del(&s->node);
        /* keep one empty slab cached, release the rest to the buddy allocator */
        if (list_empty(&c->empty)) list_add(&c->empty, &s->node);
        else {
            struct page *head = phys_to_page(VIRT_TO_PHYS(s));
            for (unsigned i = 0; i < (1u << c->slab_order); i++) { head[i].flags &= ~PG_SLAB; head[i].slab = nullptr; }
            c->nr_slabs--;
            page_free(head, c->slab_order);
        }
    } else if (was_full) {
        list_del(&s->node);
        list_add(&c->partial, &s->node);
    }
    spin_unlock_irqrestore(&c->lock, f);
}

void slab_init(void) {
    kmem_cache_init(&cache_cache, "kmem_cache", sizeof(struct kmem_cache), 8);
    for (unsigned i = 0; i < ARRAY_SIZE(kmalloc_caches); i++)
        kmem_cache_init(&kmalloc_caches[i], kmalloc_names[i], 1UL << (i + KMALLOC_MIN_SHIFT), 16);
    pr_info("slab: %zu kmalloc caches (16..2048 bytes)\n", ARRAY_SIZE(kmalloc_caches));
}

void *kmalloc(size_t size) {
    if (size == 0) size = 1;
    if (size <= (1UL << KMALLOC_MAX_SHIFT)) {
        unsigned shift = KMALLOC_MIN_SHIFT;
        while ((1UL << shift) < size) shift++;
        return kmem_cache_alloc(&kmalloc_caches[shift - KMALLOC_MIN_SHIFT]);
    }
    unsigned order = size_to_order(size);
    struct page *pg = page_alloc(order);
    if (!pg) return nullptr;
    pg->flags |= PG_LARGE;
    return PHYS_TO_VIRT(page_to_phys(pg));
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

static size_t ksize(void *p) {
    struct page *pg = phys_to_page(VIRT_TO_PHYS(p));
    if (pg->flags & PG_SLAB) return pg->slab->cache->obj_size;
    return PAGE_SIZE << pg->order;
}

void kfree(void *p) {
    if (!p) return;
    struct page *pg = phys_to_page(VIRT_TO_PHYS(p));
    if (pg->flags & PG_SLAB) { kmem_cache_free(pg->slab->cache, p); return; }
    assert(pg->flags & PG_LARGE);
    pg->flags &= ~PG_LARGE;
    page_free(pg, pg->order);
}

void *krealloc(void *p, size_t size) {
    if (!p) return kmalloc(size);
    size_t old = ksize(p);
    if (size <= old) return p;
    void *n = kmalloc(size);
    if (!n) return nullptr;
    memcpy(n, p, old);
    kfree(p);
    return n;
}

void slab_selftest(void) {
    void *ptrs[512];
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 512; i++) {
            size_t sz = 8 + (i * 37) % 5000;
            ptrs[i] = kmalloc(sz);
            assert(ptrs[i]);
            memset(ptrs[i], i & 0xff, sz);
        }
        for (int i = 0; i < 512; i++) kfree(ptrs[i]);
    }
    pr_info("slab: self-test passed\n");
}
