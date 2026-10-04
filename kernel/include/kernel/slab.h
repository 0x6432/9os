#pragma once
#include <kernel/types.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>

struct kmem_cache {
    const char *name;
    size_t obj_size;          /* aligned object size */
    unsigned slab_order;      /* buddy order of each slab */
    unsigned objs_per_slab;
    size_t first_off;         /* offset of first object from slab start */
    struct list_node partial, full, empty;
    uint64_t nr_slabs, nr_active;
    spinlock_t lock;
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
