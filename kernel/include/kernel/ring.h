#pragma once
#include <kernel/types.h>

/* Socket byte rings use independent pages, not high-order contiguous allocations.
 * Bytes [head, head+len) wrap at cap, including a partial final page. */
struct ring { uint8_t **pages; size_t cap, head, len; };
bool ring_alloc(struct ring *r, size_t cap);
void ring_free(struct ring *r);
size_t ring_space(const struct ring *r);
size_t ring_put(struct ring *r, const uint8_t *src, size_t n);
size_t ring_get(struct ring *r, uint8_t *dst, size_t n, size_t skip, bool consume);
void ring_drop(struct ring *r, size_t n);
