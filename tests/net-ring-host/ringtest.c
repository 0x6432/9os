/* Exercise the real kernel ring with a fragmented-memory allocator (no allocation
 * above 16 KiB), injected OOM, and a linear reference model. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <kernel/ring.h>

static long fail_after = -1;
static size_t live, checks;
void *kmalloc(size_t n) {
    if (n > 16384 || fail_after == 0) return NULL;
    if (fail_after > 0) fail_after--;
    void *p = malloc(n);
    if (p) live++;
    return p;
}
void kfree(void *p) { if (p) { assert(live); live--; free(p); } }
#include "../../kernel/net/ring.c"
#define CHECK(x) do { assert(x); checks++; } while (0)
static uint64_t rnd = 0x123456789abcdefULL;
static uint32_t random32(void) { rnd ^= rnd << 13; rnd ^= rnd >> 7; rnd ^= rnd << 17; return (uint32_t)rnd; }

static void model(size_t cap) {
    struct ring r = {0};
    uint8_t *ref = malloc(cap), *in = malloc(cap), *out = malloc(cap);
    CHECK(ref && in && out);
    for (size_t i = 0; i < cap; i++) in[i] = (uint8_t)(i * 37 + (i >> 8));
    CHECK(ring_alloc(&r, cap));
    size_t len = 0;
    /* Fill, cross page boundaries, wrap at a non-page-aligned capacity, peek. */
    CHECK(ring_put(&r, in, cap) == cap); memcpy(ref, in, cap); len = cap;
    size_t drop = cap / 3;
    ring_drop(&r, drop); memmove(ref, ref + drop, len - drop); len -= drop;
    CHECK(ring_put(&r, in, drop) == drop); memcpy(ref + len, in, drop); len += drop;
    CHECK(ring_get(&r, out, cap, 0, false) == cap && !memcmp(out, ref, cap));
    for (int it = 0; it < 20000; it++) {
        unsigned op = random32() % 3;
        size_t n = random32() % (cap < 25000 ? cap + 1 : 25001);
        if (op == 0) {
            size_t want = MIN(n, cap - len);
            CHECK(ring_put(&r, in, n) == want);
            memcpy(ref + len, in, want); len += want;
        } else if (op == 1) {
            size_t skip = random32() % (len + 1);
            bool consume = random32() & 1;
            size_t want = MIN(n, len - skip);
            CHECK(ring_get(&r, out, n, skip, consume) == want);
            CHECK(!memcmp(out, ref + skip, want));
            if (consume && skip < len) {
                size_t eaten = skip + want;
                memmove(ref, ref + eaten, len - eaten); len -= eaten;
            }
        } else {
            size_t want = MIN(n, len);
            ring_drop(&r, n); memmove(ref, ref + want, len - want); len -= want;
        }
        CHECK(r.len == len && ring_space(&r) == cap - len);
        CHECK(r.head < cap && (len || !r.head));
    }
    ring_free(&r); ring_free(&r);
    CHECK(!r.pages && !r.cap && !r.head && !r.len && !live);
    free(ref); free(in); free(out);
}
static void failure(void) {
    for (long f = 0; f < 4; f++) {
        struct ring r = {0}; fail_after = f;
        CHECK(!ring_alloc(&r, 2 * PAGE_SIZE + 17));
        CHECK(!live && !r.pages && !r.cap && !r.head && !r.len);
        ring_free(&r);
    }
    struct ring r = {0}; fail_after = 2048;
    CHECK(!ring_alloc(&r, 8UL << 20)); CHECK(!live && !r.pages);
    fail_after = -1;
    CHECK(!ring_alloc(&r, 0)); CHECK(!ring_alloc(&r, SIZE_MAX)); CHECK(!live);
    CHECK(ring_put(&r, NULL, 0) == 0 && ring_get(&r, NULL, 0, 0, true) == 0);
    ring_drop(&r, SIZE_MAX); ring_free(&r);
}
int main(void) {
    failure();
    const size_t caps[] = {1, 17, 4095, 4096, 4097, 12317, 2UL << 20, 8UL << 20};
    for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++) model(caps[i]);
    printf("net-ring-host: %zu checks passed (fragmentation, wrap, peek, skip, consume, OOM cleanup)\n", checks);
}
