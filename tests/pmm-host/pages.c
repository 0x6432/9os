/* Actual buddy/cache implementation, synthetic physical-page metadata, host IRQ/IPI
 * shims. No kernel memory mappings are needed for allocation/coalescing tests. */
#define sched_yield kernel_sched_yield
#include "../../kernel/mm/buddy.c"
#undef sched_yield
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

struct cpu cpus[MAX_CPUS];
int ncpus = 4;
_Thread_local int test_cpu;
static struct page metadata[512];
static int owners[512], checks;
static int pause_drain, drain_ready, release_drain, waiter_ready, pressure_done;
static struct page *pressure_result;
#define CHECK(c) do { __atomic_fetch_add(&checks, 1, __ATOMIC_RELAXED); if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); abort(); } } while (0)
void lockdep_acquire(const struct lock_class *c, bool sleeping) { (void)c; (void)sleeping; }
void lockdep_release(const struct lock_class *c, bool sleeping) { (void)c; (void)sleeping; }
void spin_lock_ipi(spinlock_t *lock) {
    if (__atomic_load_n(&pause_drain, __ATOMIC_RELAXED)) {
        if (test_cpu == 1 && lock == &buddy_lock) {
            __atomic_store_n(&drain_ready, 1, __ATOMIC_RELEASE);
            while (!__atomic_load_n(&release_drain, __ATOMIC_ACQUIRE)) sched_yield();
        }
        if (test_cpu == 2 && lock == &cache_drain_lock)
            __atomic_store_n(&waiter_ready, 1, __ATOMIC_RELEASE);
    }
    spin_lock(lock);
}
void printk(const char *fmt, ...) {}
void panic(const char *fmt, ...) {
    va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args);
    fputc('\n', stderr); abort();
}

static void reset(void) {
    test_cpu = 0;
    page_array = metadata;
    first_pfn = 0; max_pfn = ARRAY_SIZE(metadata);
    total_pages = free_pages = cached_pages = 0;
    pcaches_enabled = false;
    buddy_lock.locked = cache_drain_lock.locked = 0;
    memset(pcaches, 0, sizeof pcaches);
    memset(free_count, 0, sizeof free_count);
    memset(owners, 0, sizeof owners);
    for (unsigned i = 0; i < MAX_ORDER; i++) list_init(&free_lists[i]);
    for (unsigned i = 0; i < ARRAY_SIZE(metadata); i++) metadata[i] = (struct page){ .flags = PG_RESERVED };
    for (int i = 0; i < ncpus; i++) { cpus[i].id = i; cpus[i].online = true; }
    add_range(128 * PAGE_SIZE, 128 * PAGE_SIZE);       /* one aligned order-7 block */
}
static void free_total(uint64_t expected) {
    uint64_t freep, total; pmm_stats(&freep, &total);
    CHECK(freep == expected && total == 128);
}
static void reuse_test(void) {
    reset();
    struct page *p = page_alloc(0);
    CHECK(p && !(p->flags & PG_PCPU));                /* bootstrap still uses buddy only */
    page_free(p, 0);
    free_total(128);
    pmm_enable_cpu_caches();
    p = page_alloc(0); CHECK(p);
    page_free(p, 0);
    CHECK(p->flags == PG_PCPU && p->refcount == 0);
    CHECK(page_alloc(0) == p);
    CHECK(p->flags == 0 && p->refcount == 1 && p->slab == nullptr);
    struct pmm_cache_stats stats; pmm_cache_stats(&stats);
    CHECK(stats.alloc_hits == 1 && stats.free_hits == 1 && stats.cached_pages == 0);
    page_free(p, 0);
    free_total(128);
    pmm_drain_cpu_caches();
    pmm_cache_stats(&stats);
    CHECK(!stats.cached_pages && stats.drained_pages == 1);
    free_total(128);
    CHECK(page_alloc(MAX_ORDER) == nullptr);
}
static void coalesce_test(void) {
    reset(); pmm_enable_cpu_caches();
    struct page *pages[128];
    for (int i = 0; i < 128; i++) { pages[i] = page_alloc(0); CHECK(pages[i]); }
    CHECK(page_alloc(0) == nullptr);
    for (int i = 0; i < 128; i++) page_free(pages[i], 0);
    struct pmm_cache_stats stats; pmm_cache_stats(&stats);
    CHECK(stats.cached_pages == PCP_HIGH);
    free_total(128);
    /* The 16 cached pages initially fragment the only large free region. */
    struct page *large = page_alloc(7);
    CHECK(large == &metadata[128] && large->order == 7 && large->refcount == 1);
    pmm_cache_stats(&stats);
    CHECK(!stats.cached_pages && stats.drained_pages == PCP_HIGH);
    free_total(0);
    page_free(large, 7);
    free_total(128);
}
static void remote_test(void) {
    reset(); pmm_enable_cpu_caches();
    struct page *pages[128];
    for (int i = 0; i < 128; i++) { pages[i] = page_alloc(0); CHECK(pages[i]); }
    test_cpu = 1;
    for (int i = 0; i < PCP_HIGH; i++) page_free(pages[i], 0);
    test_cpu = 0;
    /* No buddy/local pages remain: an order-0 miss must recover CPU1's cache. */
    struct page *p = page_alloc(0);
    CHECK(p && p->flags == 0 && p->refcount == 1);
    struct pmm_cache_stats stats; pmm_cache_stats(&stats);
    CHECK(stats.cached_pages == 0 && stats.drained_pages == PCP_HIGH);
    free_total(PCP_HIGH - 1);
    for (int i = PCP_HIGH; i < 128; i++) page_free(pages[i], 0);
    page_free(p, 0);
    pmm_drain_cpu_caches();
    free_total(128);
    p = page_alloc(7); CHECK(p);
    page_free(p, 7);
}
static void await(int *flag) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec > 5) { CHECK(false); }
        sched_yield();
    }
}
static void *paused_drainer(void *arg) {
    test_cpu = 1;
    pmm_drain_cpu_caches();
    return 0;
}
static void *pressure_allocator(void *arg) {
    test_cpu = 2;
    pressure_result = page_alloc(7);
    __atomic_store_n(&pressure_done, 1, __ATOMIC_RELEASE);
    return 0;
}
static void overlapping_drain_test(void) {
    reset(); pmm_enable_cpu_caches();
    struct page *pages[128];
    for (int i = 0; i < 128; i++) { pages[i] = page_alloc(0); CHECK(pages[i]); }
    for (int i = 0; i < 128; i++) page_free(pages[i], 0);
    pause_drain = 1;
    pthread_t drainer, allocator;
    CHECK(pthread_create(&drainer, 0, paused_drainer, 0) == 0);
    await(&drain_ready);           /* CPU1 has detached pages but not yet coalesced them */
    CHECK(cache_drain_lock.locked == 1);
    free_total(128);
    CHECK(pthread_create(&allocator, 0, pressure_allocator, 0) == 0);
    await(&waiter_ready);          /* CPU2's large-allocation miss must wait for that drain */
    CHECK(!__atomic_load_n(&pressure_done, __ATOMIC_ACQUIRE));
    __atomic_store_n(&release_drain, 1, __ATOMIC_RELEASE);
    CHECK(pthread_join(drainer, 0) == 0);
    CHECK(pthread_join(allocator, 0) == 0);
    pause_drain = 0;
    CHECK(pressure_result == &metadata[128]);
    page_free(pressure_result, 7);
    free_total(128);
}
static void *churn(void *arg) {
    test_cpu = (int)(long)arg;
    for (int round = 0; round < 200; round++) {
        struct page *pages[8];
        for (int i = 0; i < 8; i++) {
            struct page *p = pages[i] = page_alloc(0);
            CHECK(p && p->flags == 0 && p->order == 0 && p->refcount == 1 && !p->slab);
            int zero = 0;
            CHECK(__atomic_compare_exchange_n(&owners[p - metadata], &zero, test_cpu + 1, false,
                                               __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
        }
        if (!(round % 17)) pmm_drain_cpu_caches();    /* simultaneous remote drains and reuse */
        for (int i = 0; i < 8; i++) {
            struct page *p = pages[i];
            CHECK(__atomic_exchange_n(&owners[p - metadata], 0, __ATOMIC_RELEASE) == test_cpu + 1);
            page_free(p, 0);
        }
    }
    return 0;
}
static void concurrent_test(void) {
    reset(); pmm_enable_cpu_caches();
    pthread_t threads[4];
    for (long i = 0; i < 4; i++) CHECK(pthread_create(&threads[i], 0, churn, (void *)i) == 0);
    for (int i = 0; i < 4; i++) CHECK(pthread_join(threads[i], 0) == 0);
    free_total(128);
    struct pmm_cache_stats stats; pmm_cache_stats(&stats);
    CHECK(stats.alloc_hits > 0 && stats.free_hits > 0 && stats.cached_pages <= 4 * PCP_HIGH);
    pmm_drain_cpu_caches();
    free_total(128);
    struct page *p = page_alloc(7); CHECK(p);
    page_free(p, 7);
    free_total(128);
}
/* Exercise the actual boot initializer with high RAM bases, an internal hole,
 * a reserved executable page, and an unaligned lower PFN. The old absolute-PFN
 * metadata allocation would ask for 14/28 MiB in this 2 MiB synthetic RAM map. */
uint64_t hhdm_offset;
static struct limine_memmap_response host_map;
struct limine_memmap_response *boot_memmap(void) { return &host_map; }
static unsigned char boot_arena[512 * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static void high_ram_test(uint64_t base) {
    reset();
    hhdm_offset = (uint64_t)boot_arena - base;
    struct limine_memmap_entry entries[] = {
        {.base = base, .length = PAGE_SIZE, .type = LIMINE_MEMMAP_EXECUTABLE_AND_MODULES},
        {.base = base + PAGE_SIZE, .length = 255 * PAGE_SIZE, .type = LIMINE_MEMMAP_USABLE},
        {.base = base + 256 * PAGE_SIZE, .length = 64 * PAGE_SIZE, .type = LIMINE_MEMMAP_RESERVED},
        {.base = base + 320 * PAGE_SIZE, .length = 192 * PAGE_SIZE, .type = LIMINE_MEMMAP_USABLE},
    };
    struct limine_memmap_entry *ep[4];
    for (int i = 0; i < 4; i++) ep[i] = &entries[i];
    host_map = (struct limine_memmap_response){.entry_count = 4, .entries = ep};
    total_pages = free_pages = cached_pages = 0;
    memset(free_count, 0, sizeof free_count);
    pmm_init();
    CHECK(first_pfn == base / PAGE_SIZE && max_pfn - first_pfn == 512);
    CHECK(page_array == (struct page *)(boot_arena + PAGE_SIZE));
    CHECK(phys_to_page(base) == page_array && page_to_phys(page_array) == base);
    CHECK(phys_to_page(base + 511 * PAGE_SIZE) == &page_array[511]);
    uint64_t metadata_pages = ALIGN_UP(512 * sizeof(struct page), PAGE_SIZE) / PAGE_SIZE;
    uint64_t expected = 447 - metadata_pages;
    uint64_t fp, tp; pmm_stats(&fp, &tp); CHECK(fp == expected && tp == expected);
    pmm_selftest();
    struct page *allocated[512]; unsigned n = 0;
    while (n < 512 && (allocated[n] = page_alloc(0))) {
        paddr_t pa = page_to_phys(allocated[n]);
        CHECK(pa >= base + (1 + metadata_pages) * PAGE_SIZE && pa < base + 512 * PAGE_SIZE);
        CHECK(pa < base + 256 * PAGE_SIZE || pa >= base + 320 * PAGE_SIZE);
        CHECK(phys_to_page(pa) == allocated[n]);
        n++;
    }
    CHECK(n == expected && page_alloc(0) == nullptr);
    while (n) page_free(allocated[--n], 0);
    pmm_stats(&fp, &tp); CHECK(fp == expected && tp == expected);
    struct page *large = page_alloc(7); CHECK(large);
    CHECK((page_to_phys(large) & (128 * PAGE_SIZE - 1)) == 0);
    page_free(large, 7);
    pmm_enable_cpu_caches();
    struct page *cached = page_alloc(0); CHECK(cached);
    page_free(cached, 0); pmm_drain_cpu_caches();
    pmm_stats(&fp, &tp); CHECK(fp == expected && tp == expected);
}
int main(void) {
    high_ram_test(0x40000000); high_ram_test(0x80000000); high_ram_test(0x80003000);
    reuse_test(); coalesce_test(); remote_test(); overlapping_drain_test(); concurrent_test();
    printf("pmm-host: %d checks passed (reuse, remote recovery, coalescing, parallel drains, high RAM bases/holes)\n", checks);
    return 0;
}