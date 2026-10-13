/* Socket rings must survive physical fragmentation after graphics/input workloads.
 * A 2 MiB receive ring used to need an order-9 buddy block, silently rejecting SYNs
 * when child allocation failed even with hundreds of MiB free. */
#include <kernel/ring.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>

bool ring_alloc(struct ring *r, size_t cap) {
    if (!cap || cap > SIZE_MAX - (PAGE_SIZE - 1)) return false;
    size_t npages = (cap + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages > SIZE_MAX / sizeof *r->pages) return false;
    uint8_t **pages = kmalloc(npages * sizeof *pages);
    if (!pages) return false;
    size_t i = 0;
    for (; i < npages; i++) {
        pages[i] = kmalloc(PAGE_SIZE);
        if (!pages[i]) break;
    }
    if (i != npages) {
        while (i) kfree(pages[--i]);
        kfree(pages);
        return false;
    }
    *r = (struct ring){ .pages = pages, .cap = cap };
    return true;
}
void ring_free(struct ring *r) {
    if (r->pages) {
        size_t npages = (r->cap + PAGE_SIZE - 1) / PAGE_SIZE;
        for (size_t i = 0; i < npages; i++) kfree(r->pages[i]);
        kfree(r->pages);
    }
    *r = (struct ring){ 0 };
}
size_t ring_space(const struct ring *r) { return r->cap - r->len; }
size_t ring_put(struct ring *r, const uint8_t *src, size_t n) {
    n = MIN(n, ring_space(r));
    size_t pos = (r->head + r->len) % (r->cap ? r->cap : 1), done = 0;
    while (done < n) {
        size_t off = pos % PAGE_SIZE;
        size_t a = MIN(n - done, MIN(PAGE_SIZE - off, r->cap - pos));
        memcpy(r->pages[pos / PAGE_SIZE] + off, src + done, a);
        done += a; pos += a;
        if (pos == r->cap) pos = 0;
    }
    r->len += n;
    return n;
}
size_t ring_get(struct ring *r, uint8_t *dst, size_t n, size_t skip, bool consume) {
    if (skip >= r->len) return 0;
    n = MIN(n, r->len - skip);
    size_t pos = (r->head + skip) % r->cap, done = 0;
    while (done < n) {
        size_t off = pos % PAGE_SIZE;
        size_t a = MIN(n - done, MIN(PAGE_SIZE - off, r->cap - pos));
        memcpy(dst + done, r->pages[pos / PAGE_SIZE] + off, a);
        done += a; pos += a;
        if (pos == r->cap) pos = 0;
    }
    if (consume) ring_drop(r, skip + n);
    return n;
}
void ring_drop(struct ring *r, size_t n) {
    n = MIN(n, r->len);
    r->head = (r->head + n) % (r->cap ? r->cap : 1);
    r->len -= n;
    if (!r->len) r->head = 0;
}
