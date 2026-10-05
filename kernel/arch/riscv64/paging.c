/* riscv64 Sv48 paging. */
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/spinlock.h>
#include <kernel/errno.h>
#include <arch/cpu.h>

#define PTE_V (1ULL << 0)
#define PTE_R (1ULL << 1)
#define PTE_W (1ULL << 2)
#define PTE_X (1ULL << 3)
#define PTE_U (1ULL << 4)
#define PTE_G (1ULL << 5)
#define PTE_A (1ULL << 6)
#define PTE_D (1ULL << 7)
#define PTE_LEAF (PTE_R | PTE_W | PTE_X)
#define PTE_PA(e) ((((e) >> 10) & ((1ULL << 44) - 1)) << 12)
#define PA_PTE(p) (((p) >> 12) << 10)
#define SATP_SV48 (9ULL << 60)

pagetable_t kernel_pt;
static spinlock_t pt_lock = SPINLOCK_INIT;
static uint64_t pt_lock_irqsave(void) { uint64_t f = arch_irq_save(); spin_lock_ipi(&pt_lock); return f; }

static uint64_t to_pte(paddr_t pa, unsigned fl) {
    /* A/D preset: we do not rely on hardware A/D updates (Svadu may be absent) */
    uint64_t e = PA_PTE(pa) | PTE_V | PTE_R | PTE_A | PTE_D;
    if (fl & VM_WRITE) e |= PTE_W;
    if (fl & VM_EXEC) e |= PTE_X;
    if (fl & VM_USER) e |= PTE_U; else e |= PTE_G;
    return e;
}

static uint64_t *walk(paddr_t root, vaddr_t va, bool create) {
    uint64_t *table = PHYS_TO_VIRT(root);
    for (int level = 3; level > 0; level--) {
        unsigned idx = (va >> (12 + 9 * level)) & 511;
        uint64_t e = table[idx];
        if (!(e & PTE_V)) {
            if (!create) return nullptr;
            paddr_t n = pmm_alloc_zeroed(0);
            if (!n) return nullptr;
            e = PA_PTE(n) | PTE_V;
            table[idx] = e;
        } else if (e & PTE_LEAF) {
            return level == 1 ? &table[idx] : nullptr;
        }
        table = PHYS_TO_VIRT(PTE_PA(e));
    }
    return &table[(va >> 12) & 511];
}

void vmm_flush(vaddr_t va) { sfence_vma(va); }

int vmm_map(pagetable_t pt, vaddr_t va, paddr_t pa, unsigned flags) {
    uint64_t f = pt_lock_irqsave();
    uint64_t *pte = walk(pt.root, va, true);
    if (!pte) { spin_unlock_irqrestore(&pt_lock, f); return -ENOMEM; }
    *pte = to_pte(pa, flags);
    if (!tlb_batched(va)) sfence_vma(va);
    spin_unlock_irqrestore(&pt_lock, f);
    return 0;
}

static int map_2m(paddr_t root, vaddr_t va, paddr_t pa, unsigned flags) {
    uint64_t *table = PHYS_TO_VIRT(root);
    for (int level = 3; level > 1; level--) {
        unsigned idx = (va >> (12 + 9 * level)) & 511;
        if (!(table[idx] & PTE_V)) {
            paddr_t n = pmm_alloc_zeroed(0);
            if (!n) return -ENOMEM;
            table[idx] = PA_PTE(n) | PTE_V;
        }
        table = PHYS_TO_VIRT(PTE_PA(table[idx]));
    }
    table[(va >> 21) & 511] = to_pte(pa, flags);
    return 0;
}

int vmm_map_range(pagetable_t pt, vaddr_t va, paddr_t pa, size_t len, unsigned flags) {
    size_t off = 0;
    while (off < len) {
        if (!(flags & VM_USER) && ((va + off) & 0x1fffff) == 0 && ((pa + off) & 0x1fffff) == 0 &&
            len - off >= 0x200000) {
            if (map_2m(pt.root, va + off, pa + off, flags)) return -ENOMEM;
            off += 0x200000;
        } else {
            if (vmm_map(pt, va + off, pa + off, flags)) return -ENOMEM;
            off += PAGE_SIZE;
        }
    }
    sfence_vma_all();
    return 0;
}

paddr_t vmm_unmap(pagetable_t pt, vaddr_t va) {
    uint64_t f = pt_lock_irqsave();
    uint64_t *pte = walk(pt.root, va, false);
    paddr_t old = 0;
    if (pte && (*pte & PTE_V)) { old = PTE_PA(*pte); *pte = 0; if (!tlb_batched(va)) { sfence_vma(va); if (va < USER_TOP) tlb_shootdown(pt.root, va); } }
    spin_unlock_irqrestore(&pt_lock, f);
    return old;
}

bool vmm_query(pagetable_t pt, vaddr_t va, paddr_t *pa, unsigned *flags) {
    uint64_t *pte = walk(pt.root, va, false);
    if (!pte || !(*pte & PTE_V)) return false;
    if (pa) *pa = PTE_PA(*pte) | (va & 0xfff);
    if (flags) {
        unsigned fl = VM_READ;
        if (*pte & PTE_W) fl |= VM_WRITE;
        if (*pte & PTE_U) fl |= VM_USER;
        if (*pte & PTE_X) fl |= VM_EXEC;
        *flags = fl;
    }
    return true;
}

int vmm_protect(pagetable_t pt, vaddr_t va, unsigned flags) {
    uint64_t *pte = walk(pt.root, va, false);
    if (!pte || !(*pte & PTE_V)) return -EFAULT;
    *pte = to_pte(PTE_PA(*pte), flags);
    sfence_vma(va);
    if (va < USER_TOP) tlb_shootdown(pt.root, va);
    return 0;
}

void *vmm_map_mmio(paddr_t pa, size_t len) {
    paddr_t base = ALIGN_DOWN(pa, PAGE_SIZE);
    size_t l = ALIGN_UP(pa + len, PAGE_SIZE) - base;
    for (size_t off = 0; off < l; off += PAGE_SIZE) {
        paddr_t cur;
        if (vmm_query(kernel_pt, (vaddr_t)PHYS_TO_VIRT(base + off), &cur, nullptr)) continue;
        vmm_map(kernel_pt, (vaddr_t)PHYS_TO_VIRT(base + off), base + off, VM_READ | VM_WRITE | VM_NOCACHE);
    }
    return PHYS_TO_VIRT(pa);
}

pagetable_t vmm_new_user_pagetable(void) {
    pagetable_t pt = { pmm_alloc_zeroed(0) };
    if (!pt.root) return pt;
    uint64_t *n = PHYS_TO_VIRT(pt.root), *k = PHYS_TO_VIRT(kernel_pt.root);
    for (int i = 256; i < 512; i++) n[i] = k[i];
    return pt;
}

static void free_level(paddr_t table, int level) {
    uint64_t *t = PHYS_TO_VIRT(table);
    for (int i = 0; i < (level == 3 ? 256 : 512); i++) {
        if (!(t[i] & PTE_V) || (t[i] & PTE_LEAF)) continue;
        if (level > 1) free_level(PTE_PA(t[i]), level - 1);
    }
    pmm_free_pages(table, 0);
}

void vmm_free_user_pagetable(pagetable_t pt) { free_level(pt.root, 3); }

void arch_tlb_flush_local(void) { sfence_vma_all(); }

/* remote fences through the SBI RFENCE extension (synchronous, no IPI handler needed) */
void arch_tlb_remote(uint64_t mask, vaddr_t va) {
    for (int i = 0; i < ncpus; i++)
        if (mask & (1ULL << i)) {
            if (va == ~0UL) sbi_call4(0x52464E43, 1, 1, (long)cpus[i].hwid, 0, -1L);   /* whole address space */
            else sbi_call4(0x52464E43, 1, 1, (long)cpus[i].hwid, (long)ALIGN_DOWN(va, PAGE_SIZE), PAGE_SIZE);
        }
}

void vmm_switch(pagetable_t pt) {
    this_cpu()->active_root = pt.root;
    uint64_t satp = SATP_SV48 | (pt.root >> 12);
    if (csr_read(satp) != satp) { csr_write(satp, satp); sfence_vma_all(); }
}

extern char __kernel_start[], __text_start[], __text_end[], __rodata_start[], __rodata_end[],
            __data_start[], __kernel_end[];

void vmm_init(void) {
    if ((csr_read(satp) >> 60) != 9) panic("vmm: bootloader did not enable Sv48 (satp %lx)", csr_read(satp));
    kernel_pt.root = pmm_alloc_zeroed(0);
    uint64_t *root = PHYS_TO_VIRT(kernel_pt.root);
    for (int i = 256; i < 512; i++) root[i] = PA_PTE(pmm_alloc_zeroed(0)) | PTE_V;

    struct limine_memmap_response *mm = boot_memmap();
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_BAD_MEMORY || e->type == LIMINE_MEMMAP_RESERVED) continue;
        paddr_t base = ALIGN_DOWN(e->base, PAGE_SIZE);
        paddr_t end = ALIGN_UP(e->base + e->length, PAGE_SIZE);
        vmm_map_range(kernel_pt, (vaddr_t)PHYS_TO_VIRT(base), base, end - base, VM_READ | VM_WRITE);
    }

    struct limine_executable_address_response *ka = boot_kernel_address();
    uint64_t vbase = ka->virtual_base, pbase = ka->physical_base;
#define KMAP(s, e, fl) vmm_map_range(kernel_pt, (vaddr_t)(s), (paddr_t)(s) - vbase + pbase, \
                                     ALIGN_UP((uint64_t)(e), PAGE_SIZE) - (uint64_t)(s), fl)
    KMAP(__kernel_start, __text_start, VM_READ);
    KMAP(__text_start, __text_end, VM_READ | VM_EXEC);
    KMAP(__rodata_start, __rodata_end, VM_READ);
    KMAP(__data_start, __kernel_end, VM_READ | VM_WRITE);

    vmm_switch(kernel_pt);
    pr_info("vmm: Sv48 kernel page tables active (root %lx)\n", kernel_pt.root);
}
