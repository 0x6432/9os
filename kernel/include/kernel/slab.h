#pragma once
#include <kernel/types.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/cpu.h>

#define SLAB_CPU_CAP 8
struct slab_magazine {
    spinlock_t lock;
    unsigned nr;
    void *objects[SLAB_CPU_CAP];
    uint64_t alloc_hits, free_hits;
} __attribute__((aligned(64)));

struct kmem_cache {
    const char *name;
    size_t obj_size;          /* aligned object size */
    unsigned slab_order;      /* buddy order of each slab */
    unsigned objs_per_slab;
    size_t first_off;         /* offset of first object from slab start */
    struct list_node partial, full, empty;
    uint64_t nr_slabs, nr_active;
    spinlock_t lock;
    struct list_node registry_node;
    uint64_t nr_cached, drained;
    struct slab_magazine cpu[MAX_CPUS];
};

struct kmem_slab {
    struct list_node node;
    struct kmem_cache *cache;
    void *freelist;
    unsigned inuse;
};

void slab_init(void);
struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align);
void kmem_cache_init(struct kmem_cache *c, const char *name, size_t size, size_t align);
void *kmem_cache_alloc(struct kmem_cache *c);
void kmem_cache_free(struct kmem_cache *c, void *obj);
void slab_selftest(void);
void slab_enable_cpu_caches(void);
void slab_drain_cpu_caches(void);
void slab_reclaim_cpu_caches(void);
struct slab_cpu_stats { uint64_t cached_objects, alloc_hits, free_hits, drained_objects; };
void slab_cpu_stats(struct slab_cpu_stats *stats);
void slab_cpu_selftest(void);
