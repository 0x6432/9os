#pragma once
#include <kernel/types.h>

/* Portable mapping flags */
enum {
    VM_READ  = 1 << 0,
    VM_WRITE = 1 << 1,
    VM_EXEC  = 1 << 2,
    VM_USER  = 1 << 3,
    VM_NOCACHE = 1 << 4,
    VM_WC    = 1 << 5,
    VM_HUGE  = 1 << 6,      /* 2 MiB leaf (internal) */
};

typedef struct { paddr_t root; } pagetable_t;

extern pagetable_t kernel_pt;

void vmm_init(void);
/* Map one 4 KiB page. Returns 0 or -ENOMEM. */
int vmm_map(pagetable_t pt, vaddr_t va, paddr_t pa, unsigned flags);
int vmm_map_range(pagetable_t pt, vaddr_t va, paddr_t pa, size_t len, unsigned flags);
/* Unmap; returns old physical address or 0. */
paddr_t vmm_unmap(pagetable_t pt, vaddr_t va);
/* Translate; returns true and fills pa/flags if mapped. */
bool vmm_query(pagetable_t pt, vaddr_t va, paddr_t *pa, unsigned *flags);
int vmm_protect(pagetable_t pt, vaddr_t va, unsigned flags);
void *vmm_map_mmio(paddr_t pa, size_t len);
pagetable_t vmm_new_user_pagetable(void);
void vmm_free_user_pagetable(pagetable_t pt);
void vmm_switch(pagetable_t pt);
void vmm_flush(vaddr_t va);
