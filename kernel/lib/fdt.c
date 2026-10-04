/* Minimal flattened-device-tree reader: look up properties by node-name prefix. */
#include <kernel/fdt.h>
#include <kernel/boot.h>
#include <kernel/string.h>

static uint32_t be32(const void *p) { const uint8_t *b = p; return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }

/* Find property `prop` in the first node whose name starts with `node` ("" = any node). */
const void *fdt_find_prop(const char *node, const char *prop, uint32_t *len) {
    const uint8_t *fdt = boot_dtb();
    if (!fdt || be32(fdt) != 0xd00dfeed) return nullptr;
    const uint8_t *st = fdt + be32(fdt + 8);
    const char *strs = (const char *)fdt + be32(fdt + 12);
    size_t nl = strlen(node);
    bool in_node = nl == 0;
    int depth = 0, match_depth = -1;
    for (;;) {
        uint32_t tok = be32(st); st += 4;
        if (tok == 1) {                                   /* FDT_BEGIN_NODE */
            const char *name = (const char *)st;
            depth++;
            if (match_depth < 0 && nl && !strncmp(name, node, nl)) { in_node = true; match_depth = depth; }
            st += ALIGN_UP(strlen(name) + 1, 4);
        } else if (tok == 2) {                            /* FDT_END_NODE */
            if (depth == match_depth) { in_node = false; match_depth = -1; }
            depth--;
        } else if (tok == 3) {                            /* FDT_PROP */
            uint32_t l = be32(st), off = be32(st + 4);
            const uint8_t *val = st + 8;
            st += 8 + ALIGN_UP(l, 4);
            if (in_node && !strcmp(strs + off, prop)) { if (len) *len = l; return val; }
        } else if (tok == 4) {                            /* FDT_NOP */
        } else break;                                     /* FDT_END or garbage */
    }
    return nullptr;
}

bool fdt_read_u32(const char *node, const char *prop, uint32_t *out) {
    uint32_t l;
    const void *v = fdt_find_prop(node, prop, &l);
    if (!v || l < 4) return false;
    *out = be32(v);
    return true;
}

/* First (address, size) pair of a node's "reg", assuming #address-cells = #size-cells = 2. */
bool fdt_read_reg(const char *node, uint64_t *addr, uint64_t *size) {
    uint32_t l;
    const uint8_t *v = fdt_find_prop(node, "reg", &l);
    if (!v || l < 16) return false;
    *addr = (uint64_t)be32(v) << 32 | be32(v + 4);
    if (size) *size = (uint64_t)be32(v + 8) << 32 | be32(v + 12);
    return true;
}
