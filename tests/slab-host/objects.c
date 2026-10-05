/* Real buddy + slab allocators over an aligned physical arena. Hardware only is
 * mocked. Concurrent magazine drains and true OOM exercise the lock ordering. */
#define sched_yield kernel_sched_yield
#include "../../kernel/mm/buddy.c"
#include "../../kernel/mm/slab.c"
#undef sched_yield
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define NPAGES 1024
static unsigned char arena[NPAGES * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static struct page metadata[NPAGES];
struct cpu cpus[MAX_CPUS];
int ncpus = 5;
_Thread_local int test_cpu;
uint64_t hhdm_offset;
static int checks, stop_drain;
static struct kmem_cache custom;
#define CHECK(c) do { __atomic_fetch_add(&checks, 1, __ATOMIC_RELAXED); if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); abort(); } } while (0)
void spin_lock_ipi(spinlock_t *lock) { spin_lock(lock); }
void printk(const char *fmt, ...) {}
void panic(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); abort();
}
static void setup(void) {
    hhdm_offset = (uint64_t)arena;
    page_array = metadata; max_pfn = NPAGES;
    for (unsigned i = 0; i < MAX_ORDER; i++) list_init(&free_lists[i]);
    for (int i = 0; i < NPAGES; i++) metadata[i].flags = PG_RESERVED;
    for (int i = 0; i < ncpus; i++) { cpus[i].id = i; cpus[i].online = true; }
    add_range(0, sizeof arena);
    slab_init(); slab_selftest();
    pmm_enable_cpu_caches(); slab_enable_cpu_caches(); slab_cpu_selftest();
}
static void sizes_test(void) {
    const size_t sizes[] = {0, 1, 15, 16, 17, 31, 63, 127, 255, 511, 1023, 2048, 2049, 4096, 8193, 65536};
    for (unsigned i = 0; i < ARRAY_SIZE(sizes); i++) {
        size_t sz = sizes[i];
        unsigned char *p = kzalloc(sz); CHECK(p && !((uintptr_t)p & 15));
        for (size_t j = 0; j < sz; j++) CHECK(!p[j]);
        memset(p, 0xA5, sz);
        unsigned char *q = krealloc(p, MAX(sz, 1) * 2); CHECK(q);
        for (size_t j = 0; j < sz; j++) CHECK(q[j] == 0xA5);
        CHECK(krealloc(q, 1) == q); kfree(q);
    }
    CHECK(kmalloc(SIZE_MAX) == nullptr);
    CHECK(kmalloc((PAGE_SIZE << (MAX_ORDER - 1)) + 1) == nullptr);
    CHECK(kmem_cache_create("bad-align", 32, 12) == nullptr);
    CHECK(kmem_cache_create("oversized", SIZE_MAX, 64) == nullptr);
    CHECK(kmem_cache_create("no-space", PAGE_SIZE << 4, 64) == nullptr);
    /* Registry lifetime is permanent (there is deliberately no destroy API). */
    kmem_cache_init(&custom, "aligned", 33, 256);
    void *p = kmem_cache_alloc(&custom); CHECK(p && !((uintptr_t)p & 255));
    test_cpu = 1; kmem_cache_free(&custom, p);
    CHECK(custom.cpu[1].nr == 1 && custom.nr_cached == 1);
    CHECK(kmem_cache_alloc(&custom) == p); kmem_cache_free(&custom, p);
    slab_drain_cpu_caches(); CHECK(custom.nr_active == 0 && custom.nr_cached == 0);
    test_cpu = 0;
}
static void pressure_test(void) {
    slab_reclaim_cpu_caches(); pmm_drain_cpu_caches();
    CHECK(free_pages == NPAGES);
    void *obj = kmalloc(64); CHECK(obj);
    test_cpu = 1; kfree(obj); test_cpu = 0;
    CHECK(kmalloc_caches[2].nr_cached == 1);
    struct page *pages[NPAGES]; int count = 0;
    while (count < NPAGES && (pages[count] = page_alloc(0))) count++;
    /* All slab/PCP memory was recovered, including a remote CPU magazine. */
    CHECK(count == NPAGES && free_pages == 0);
    CHECK(kmalloc_caches[2].nr_cached == 0 && kmalloc_caches[2].nr_slabs == 0);
    CHECK(kmalloc(64) == nullptr && kmalloc(8192) == nullptr);
    CHECK(kmem_cache_create("oom-metadata", 32, 64) == nullptr);
    for (int i = 0; i < count; i++) page_free(pages[i], 0);
    pmm_drain_cpu_caches(); CHECK(free_pages == NPAGES);
    /* Large-order pressure must drain magazines and coalesce their backing pages. */
    obj = kmalloc(128); CHECK(obj); kfree(obj);
    struct page *large = page_alloc(MAX_ORDER - 1);
    CHECK(large == metadata && free_pages == 0); page_free(large, MAX_ORDER - 1);
}
static void *worker(void *arg) {
    test_cpu = (int)(intptr_t)arg;
    for (int round = 0; round < 500; round++) {
        unsigned char *objects[32]; size_t sizes[32];
        for (int i = 0; i < 32; i++) {
            sizes[i] = 16U << ((round + i) % 8);
            objects[i] = kzalloc(sizes[i]); CHECK(objects[i]);
            for (size_t j = 0; j < sizes[i]; j++) CHECK(!objects[i][j]);
            memset(objects[i], test_cpu + 1, sizes[i]);
        }
        for (int i = 31; i >= 0; i--) {
            for (size_t j = 0; j < sizes[i]; j++) CHECK(objects[i][j] == test_cpu + 1);
            kfree(objects[i]);
        }
    }
    return 0;
}
static void *drainer(void *arg) {
    test_cpu = 4;
    while (!__atomic_load_n(&stop_drain, __ATOMIC_ACQUIRE)) {
        slab_reclaim_cpu_caches(); pmm_drain_cpu_caches(); sched_yield();
    }
    return 0;
}
static void concurrent_test(void) {
    pthread_t w[4], d;
    CHECK(pthread_create(&d, 0, drainer, 0) == 0);
    for (intptr_t i = 0; i < 4; i++) CHECK(pthread_create(&w[i], 0, worker, (void *)i) == 0);
    for (int i = 0; i < 4; i++) CHECK(pthread_join(w[i], 0) == 0);
    __atomic_store_n(&stop_drain, 1, __ATOMIC_RELEASE);
    CHECK(pthread_join(d, 0) == 0);
    slab_reclaim_cpu_caches(); pmm_drain_cpu_caches(); CHECK(free_pages == NPAGES);
    struct slab_cpu_stats s; slab_cpu_stats(&s);
    CHECK(!s.cached_objects && s.alloc_hits > 0 && s.free_hits > 0 && s.drained_objects > 0);
    for (unsigned i = 0; i < ARRAY_SIZE(kmalloc_caches); i++)
        CHECK(!kmalloc_caches[i].nr_active && !kmalloc_caches[i].nr_slabs);
}
static void bounds_metadata_test(void) {
    void *objects[SLAB_CPU_CAP * 3];
    for (unsigned i = 0; i < ARRAY_SIZE(objects); i++) { objects[i] = kmalloc(64); CHECK(objects[i]); }
    for (unsigned i = 0; i < ARRAY_SIZE(objects); i++) kfree(objects[i]);
    CHECK(kmalloc_caches[2].cpu[0].nr == SLAB_CPU_CAP);
    CHECK(kmalloc_caches[2].nr_cached == SLAB_CPU_CAP && kmalloc_caches[2].nr_active == SLAB_CPU_CAP);
    slab_reclaim_cpu_caches(); pmm_drain_cpu_caches(); CHECK(free_pages == NPAGES);
    struct kmem_cache *c = kmem_cache_create("dynamic-large", 5000, 64);
    CHECK(c && !((uintptr_t)c & (_Alignof(struct kmem_cache) - 1)));
    CHECK(c->obj_size > 2048 && c->objs_per_slab >= 8);
    void *p = kmem_cache_alloc(c); CHECK(p && !((uintptr_t)p & 63));
    kmem_cache_free(c, p); CHECK(!c->nr_cached && !c->nr_active);
    slab_reclaim_cpu_caches(); pmm_drain_cpu_caches();
    CHECK(cache_cache.nr_active == 1 && !cache_cache.nr_cached);
    CHECK(free_pages == NPAGES - (1ULL << cache_cache.slab_order));
}
int main(void) {
    setup(); sizes_test(); pressure_test(); concurrent_test(); bounds_metadata_test();
    printf("slab-host: %d checks passed (size/alignment/zeroing/realloc/remote reuse/OOM/coalescing/concurrent drains)\n", checks);
    return 0;
}
