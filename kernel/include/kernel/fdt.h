#pragma once
#include <kernel/types.h>
const void *fdt_find_prop(const char *node, const char *prop, uint32_t *len);
bool fdt_read_u32(const char *node, const char *prop, uint32_t *out);
bool fdt_read_reg(const char *node, uint64_t *addr, uint64_t *size);
