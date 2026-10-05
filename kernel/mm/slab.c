/* Slab allocator and kmalloc front-end. */
#include <kernel/slab.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/kmalloc.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/sched.h>

#define KMALLOC_MIN_SHIFT 4     /* 16 bytes */
#define KMALLOC_MAX_SHIFT 11    /* 2048 bytes */
static struct kmem_cache kmalloc_caches[KMALLOC_MAX_SHIFT - KMALLOC_MIN_SHIFT + 1];
static const char *kmalloc_names[] = { "kmalloc-16", "kmalloc-32", "kmalloc-64", "kmalloc-128",
                                       "kmalloc-256", "kmalloc-512", "kmalloc-1024", "kmalloc-2048" };
static struct kmem_cache cache_cache;   /* cache of kmem_cache structs */
static struct list_node all_caches = LIST_INIT(all_caches);
static spinlock_t registry_lock = SPINLOCK_INIT;
static bool cpu_caches_enabled;

static uint64_t cache_lock(spinlock_t *lock) {
    uint64_t f = arch_irq_save();
    if (__atomic_load_n(&cpu_caches_enabled, __ATOMIC_ACQUIRE)) spin_lock_ipi(lock);
    else spin_lock(lock);
    return f;
}
static bool magazine_enabled(struct kmem_cache *c) {
    return c->obj_size <= 2048 && __atomic_load_n(&cpu_caches_enabled, __ATOMIC_ACQUIRE);
}

void kmem_cache_init(struct kmem_cache *c, const char *name, size_t size, size_t align) {
    if (align < sizeof(void *)) align = sizeof(void *);
    assert(!(align & (align - 1)));
    memset(c, 0, sizeof *c);
    c->name = name;
    c->obj_size = ALIGN_UP(MAX(size, sizeof(void *)), align);
    c->first_off = ALIGN_UP(sizeof(struct kmem_slab), align);
    /* choose a slab order so that at least 8 objects fit and waste is small */
    unsigned order = 0;
    while (order < 4 && ((PAGE_SIZE << order) - c->first_off) / c->obj_size < 8) order++;
    c->slab_order = order;
    c->objs_per_slab = ((PAGE_SIZE << order) - c->first_off) / c->obj_size;
    assert(c->objs_per_slab);
    list_init(&c->partial); list_init(&c->full); list_init(&c->empty);
    uint64_t f = cache_lock(&registry_lock);
    list_add_tail(&all_caches, &c->registry_node);
    spin_unlock_irqrestore(&registry_lock, f);
}

struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align) {
    size_t a = MAX(align, sizeof(void *)), limit = PAGE_SIZE << 4;
    if ((a & (a - 1)) || a > limit || size > limit ||
        ALIGN_UP(MAX(size, sizeof(void *)), a) > limit - ALIGN_UP(sizeof(struct kmem_slab), a)) return nullptr;
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
        __atomic_fetch_or(&pg[i].flags, PG_SLAB, __ATOMIC_RELAXED);
        pg[i].slab = s;
    }
    return s;
}

static void *cache_alloc_global(struct kmem_cache *c) {
    uint64_t f = cache_lock(&c->lock);
    struct kmem_slab *s;
    if (!list_empty(&c->partial)) s = list_first(&c->partial, struct kmem_slab, node);
    else if (!list_empty(&c->empty)) {
        s = list_first(&c->empty, struct kmem_slab, node);
        list_del(&s->node);
        list_add(&c->partial, &s->node);
    } else {
        /* No cache/registry/magazine lock may span page_alloc: its pressure
         * hook can reclaim magazines and empty slabs from this very cache. */
        spin_unlock_irqrestore(&c->lock, f);
        s = slab_grow(c);
        if (!s) return nullptr;
        f = cache_lock(&c->lock);
        __atomic_fetch_add(&c->nr_slabs, 1, __ATOMIC_RELAXED);
        list_add(&c->partial, &s->node);
    }
    void **obj = s->freelist;
    s->freelist = *obj;
    s->inuse++;
    __atomic_fetch_add(&c->nr_active, 1, __ATOMIC_RELAXED);
    if (s->inuse == c->objs_per_slab) { list_del(&s->node); list_add(&c->full, &s->node); }
    spin_unlock_irqrestore(&c->lock, f);
    return obj;
}

static void cache_free_global(struct kmem_cache *c, void *obj) {
    struct page *pg = phys_to_page(VIRT_TO_PHYS(obj));
    struct kmem_slab *s = pg->slab;
    assert((pg->flags & PG_SLAB) && s->cache == c);
    uint64_t f = cache_lock(&c->lock);
    bool was_full = s->inuse == c->objs_per_slab;
    *(void **)obj = s->freelist;
    s->freelist = obj;
    s->inuse--;
    __atomic_fetch_sub(&c->nr_active, 1, __ATOMIC_RELAXED);
    if (s->inuse == 0) {
        list_del(&s->node);
        /* keep one empty slab cached, release the rest to the buddy allocator */
        if (list_empty(&c->empty)) list_add(&c->empty, &s->node);
        else {
            struct page *head = phys_to_page(VIRT_TO_PHYS(s));
            for (unsigned i = 0; i < (1u << c->slab_order); i++) { __atomic_fetch_and(&head[i].flags, ~PG_SLAB, __ATOMIC_RELAXED); head[i].slab = nullptr; }
            __atomic_fetch_sub(&c->nr_slabs, 1, __ATOMIC_RELAXED);
            page_free(head, c->slab_order);
        }
    } else if (was_full) {
        list_del(&s->node);
        list_add(&c->partial, &s->node);
    }
    spin_unlock_irqrestore(&c->lock, f);
}

void *kmem_cache_alloc(struct kmem_cache *c) {
    if (magazine_enabled(c)) {
        uint64_t f = arch_irq_save();
        struct slab_magazine *m = &c->cpu[this_cpu()->id];
        spin_lock_ipi(&m->lock);
        if (m->nr) {
            void *obj = m->objects[--m->nr];
            __atomic_fetch_sub(&c->nr_cached, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&m->alloc_hits, 1, __ATOMIC_RELAXED);
            spin_unlock_irqrestore(&m->lock, f);
            return obj;
        }
        spin_unlock_irqrestore(&m->lock, f);
    }
    return cache_alloc_global(c);
}

void kmem_cache_free(struct kmem_cache *c, void *obj) {
    if (magazine_enabled(c)) {
        struct page *pg = phys_to_page(VIRT_TO_PHYS(obj));
        assert((pg->flags & PG_SLAB) && pg->slab->cache == c);
        uint64_t f = arch_irq_save();
        struct slab_magazine *m = &c->cpu[this_cpu()->id];
        spin_lock_ipi(&m->lock);
        if (m->nr < SLAB_CPU_CAP) {
            m->objects[m->nr++] = obj;
            __atomic_fetch_add(&c->nr_cached, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&m->free_hits, 1, __ATOMIC_RELAXED);
            spin_unlock_irqrestore(&m->lock, f);
            return;
        }
        spin_unlock_irqrestore(&m->lock, f);
    }
    cache_free_global(c, obj);
}

void slab_enable_cpu_caches(void) {
    assert(!cpu_caches_enabled);
    __atomic_store_n(&cpu_caches_enabled, true, __ATOMIC_RELEASE);
    pr_info("slab: per-CPU magazines enabled (%d objects/cache/CPU, objects <=2048 bytes)\n", SLAB_CPU_CAP);
}

static void drain_caches(bool trim) {
    if (!__atomic_load_n(&cpu_caches_enabled, __ATOMIC_ACQUIRE)) return;
    uint64_t f = cache_lock(&registry_lock);
    list_for_each(it, &all_caches) {
        struct kmem_cache *c = list_entry(it, struct kmem_cache, registry_node);
        for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
            struct slab_magazine *m = &c->cpu[cpu];
            void *objects[SLAB_CPU_CAP];
            spin_lock_ipi(&m->lock);
            unsigned nr = m->nr;
            for (unsigned i = 0; i < nr; i++) objects[i] = m->objects[i];
            m->nr = 0;
            __atomic_fetch_sub(&c->nr_cached, nr, __ATOMIC_RELAXED);
            spin_unlock(&m->lock);
            /* Objects still reserve slab slots while detached, so the slab
             * cannot be freed until these references reach the global path. */
            for (unsigned i = 0; i < nr; i++) cache_free_global(c, objects[i]);
            __atomic_fetch_add(&c->drained, nr, __ATOMIC_RELAXED);
        }
        if (trim) {
            spin_lock_ipi(&c->lock);
            list_for_each_safe(n, tmp, &c->empty) {
                struct kmem_slab *s = list_entry(n, struct kmem_slab, node);
                struct page *head = phys_to_page(VIRT_TO_PHYS(s));
                list_del(n);
                for (unsigned i = 0; i < (1u << c->slab_order); i++) {
                    __atomic_fetch_and(&head[i].flags, ~PG_SLAB, __ATOMIC_RELAXED);
                    head[i].slab = nullptr;
                }
                __atomic_fetch_sub(&c->nr_slabs, 1, __ATOMIC_RELAXED);
                page_free(head, c->slab_order);
            }
            spin_unlock(&c->lock);
        }
    }
    spin_unlock_irqrestore(&registry_lock, f);
}
void slab_drain_cpu_caches(void) { drain_caches(false); }
void slab_reclaim_cpu_caches(void) { drain_caches(true); }

void slab_cpu_stats(struct slab_cpu_stats *stats) {
    memset(stats, 0, sizeof *stats);
    uint64_t f = cache_lock(&registry_lock);
    list_for_each(it, &all_caches) {
        struct kmem_cache *c = list_entry(it, struct kmem_cache, registry_node);
        stats->cached_objects += __atomic_load_n(&c->nr_cached, __ATOMIC_RELAXED);
        stats->drained_objects += __atomic_load_n(&c->drained, __ATOMIC_RELAXED);
        for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
            stats->alloc_hits += __atomic_load_n(&c->cpu[cpu].alloc_hits, __ATOMIC_RELAXED);
            stats->free_hits += __atomic_load_n(&c->cpu[cpu].free_hits, __ATOMIC_RELAXED);
        }
    }
    spin_unlock_irqrestore(&registry_lock, f);
}

void slab_cpu_selftest(void) {
    for (int i = 0; i < 128; i++) {
        void *p = kzalloc(128); assert(p);
        for (int j = 0; j < 128; j++) assert(!((uint8_t *)p)[j]);
        memset(p, 0xA5, 128); kfree(p);
    }
    struct slab_cpu_stats s; slab_cpu_stats(&s);
    assert(s.alloc_hits && s.free_hits);
    slab_drain_cpu_caches();
    pr_info("slab: per-CPU magazine self-test passed\n");
}

void slab_init(void) {
    kmem_cache_init(&cache_cache, "kmem_cache", sizeof(struct kmem_cache), _Alignof(struct kmem_cache));
    for (unsigned i = 0; i < ARRAY_SIZE(kmalloc_caches); i++)
        kmem_cache_init(&kmalloc_caches[i], kmalloc_names[i], 1UL << (i + KMALLOC_MIN_SHIFT), 16);
    pr_info("slab: %zu kmalloc caches (16..2048 bytes)\n", ARRAY_SIZE(kmalloc_caches));
}

void *kmalloc(size_t size) {
    if (size > (PAGE_SIZE << (MAX_ORDER - 1))) return nullptr;
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
