/* x86_64 4-level paging. */
#include <kernel/sched.h>
#include <kernel/vmm.h>
#include <kernel/mm.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/spinlock.h>
#include <kernel/errno.h>
#include <arch/cpu.h>

#define PTE_P   (1ULL << 0)
#define PTE_W   (1ULL << 1)
#define PTE_U   (1ULL << 2)
#define PTE_PWT (1ULL << 3)
#define PTE_PCD (1ULL << 4)
#define PTE_PS  (1ULL << 7)
#define PTE_G   (1ULL << 8)
#define PTE_PAT4K (1ULL << 7)
#define PTE_NX  (1ULL << 63)
#define PTE_ADDR 0x000ffffffffff000ULL

pagetable_t kernel_pt;
static spinlock_t pt_lock = SPINLOCK_INIT;
static uint64_t pt_lock_irqsave(void) { uint64_t f = arch_irq_save(); spin_lock_ipi(&pt_lock); return f; }

static uint64_t to_pte(paddr_t pa, unsigned fl) {
    uint64_t e = (pa & PTE_ADDR) | PTE_P;
    if (fl & VM_WRITE) e |= PTE_W;
    if (fl & VM_USER) e |= PTE_U;
    if (!(fl & VM_EXEC)) e |= PTE_NX;
    if (fl & VM_NOCACHE) e |= PTE_PCD | PTE_PWT;
    if (fl & VM_WC) e |= PTE_PWT;   /* PAT entry 1 reprogrammed to WC */
    if (!(fl & VM_USER)) e |= PTE_G;
    return e;
}

static uint64_t *walk(paddr_t root, vaddr_t va, bool create, bool user) {
    uint64_t *table = PHYS_TO_VIRT(root);
    for (int level = 3; level > 0; level--) {
        unsigned idx = (va >> (12 + 9 * level)) & 511;
        uint64_t e = table[idx];
        if (!(e & PTE_P)) {
            if (!create) return nullptr;
            paddr_t n = pmm_alloc_zeroed(0);
            if (!n) return nullptr;
            e = n | PTE_P | PTE_W | (user ? PTE_U : 0);
            table[idx] = e;
        } else if (e & PTE_PS) {
            return level == 1 ? &table[idx] : nullptr;
        }
        if (user && !(e & PTE_U)) table[idx] |= PTE_U;
        table = PHYS_TO_VIRT(e & PTE_ADDR);
    }
    return &table[(va >> 12) & 511];
}

void vmm_flush(vaddr_t va) { invlpg(va); }

int vmm_map(pagetable_t pt, vaddr_t va, paddr_t pa, unsigned flags) {
    uint64_t f = pt_lock_irqsave();
    uint64_t *pte = walk(pt.root, va, true, flags & VM_USER);
    if (!pte) { spin_unlock_irqrestore(&pt_lock, f); return -ENOMEM; }
    *pte = to_pte(pa, flags);
    if (!tlb_batched(va)) invlpg(va);
    spin_unlock_irqrestore(&pt_lock, f);
    return 0;
}

static int map_2m(paddr_t root, vaddr_t va, paddr_t pa, unsigned flags) {
    uint64_t *table = PHYS_TO_VIRT(root);
    for (int level = 3; level > 1; level--) {
        unsigned idx = (va >> (12 + 9 * level)) & 511;
        if (!(table[idx] & PTE_P)) {
            paddr_t n = pmm_alloc_zeroed(0);
            if (!n) return -ENOMEM;
            table[idx] = n | PTE_P | PTE_W;
        }
        table = PHYS_TO_VIRT(table[idx] & PTE_ADDR);
    }
    uint64_t e = to_pte(pa, flags) | PTE_PS;
    if (flags & VM_WC) { e &= ~PTE_PWT; e |= PTE_PWT; }
    table[(va >> 21) & 511] = e;
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
    return 0;
}

paddr_t vmm_unmap(pagetable_t pt, vaddr_t va) {
    uint64_t f = pt_lock_irqsave();
    uint64_t *pte = walk(pt.root, va, false, false);
    paddr_t old = 0;
    if (pte && (*pte & PTE_P)) { old = *pte & PTE_ADDR; *pte = 0; if (!tlb_batched(va)) { invlpg(va); if (va < USER_TOP) tlb_shootdown(pt.root, va); } }
    spin_unlock_irqrestore(&pt_lock, f);
    return old;
}

bool vmm_query(pagetable_t pt, vaddr_t va, paddr_t *pa, unsigned *flags) {
    uint64_t *pte = walk(pt.root, va, false, false);
    if (!pte || !(*pte & PTE_P)) return false;
    if (pa) *pa = (*pte & PTE_ADDR) | (va & 0xfff);
    if (flags) {
        unsigned fl = VM_READ;
        if (*pte & PTE_W) fl |= VM_WRITE;
        if (*pte & PTE_U) fl |= VM_USER;
        if (!(*pte & PTE_NX)) fl |= VM_EXEC;
        *flags = fl;
    }
    return true;
}

int vmm_protect(pagetable_t pt, vaddr_t va, unsigned flags) {
    uint64_t *pte = walk(pt.root, va, false, false);
    if (!pte || !(*pte & PTE_P)) return -EFAULT;
    *pte = to_pte(*pte & PTE_ADDR, flags);
    if (tlb_batched(va)) return 0;
    invlpg(va);
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
        if (!(t[i] & PTE_P) || (t[i] & PTE_PS)) continue;
        if (level > 1) free_level(t[i] & PTE_ADDR, level - 1);
    }
    pmm_free_pages(table, 0);
}

/* Frees only the page-table pages; caller must have released the mapped frames. */
void vmm_free_user_pagetable(pagetable_t pt) { free_level(pt.root, 3); }

void vmm_switch(pagetable_t pt) {
    this_cpu()->active_root = pt.root;
    if (read_cr3() != pt.root) write_cr3(pt.root);
}

void arch_tlb_flush_local(void) { write_cr3(read_cr3()); }

/* the AP loads the same CR0/EFER/PAT setup as the boot CPU did in vmm_init */
void x86_ap_paging_init(void) {
    wrmsr(0xC0000080, rdmsr(0xC0000080) | (1 << 11));
    write_cr0(read_cr0() | (1 << 16));
    uint64_t pat = rdmsr(0x277);
    pat = (pat & ~(0xffULL << 8)) | (0x01ULL << 8);
    wrmsr(0x277, pat);
    vmm_switch(kernel_pt);
}

extern char __kernel_start[], __text_start[], __text_end[], __rodata_start[], __rodata_end[],
            __data_start[], __kernel_end[];

void vmm_init(void) {
    /* enable NX and write protect; program PAT entry 1 as write-combining */
    wrmsr(0xC0000080, rdmsr(0xC0000080) | (1 << 11));
    write_cr0(read_cr0() | (1 << 16));
    uint64_t pat = rdmsr(0x277);
    pat = (pat & ~(0xffULL << 8)) | (0x01ULL << 8);
    wrmsr(0x277, pat);

    kernel_pt.root = pmm_alloc_zeroed(0);
    /* pre-create all upper-half PML4 entries so they are shared by every address space */
    uint64_t *pml4 = PHYS_TO_VIRT(kernel_pt.root);
    for (int i = 256; i < 512; i++) pml4[i] = pmm_alloc_zeroed(0) | PTE_P | PTE_W;

    struct limine_memmap_response *mm = boot_memmap();
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_BAD_MEMORY || e->type == LIMINE_MEMMAP_RESERVED) continue;
        paddr_t base = ALIGN_DOWN(e->base, PAGE_SIZE);
        paddr_t end = ALIGN_UP(e->base + e->length, PAGE_SIZE);
        unsigned fl = VM_READ | VM_WRITE;
        if (e->type == LIMINE_MEMMAP_FRAMEBUFFER) fl |= VM_WC;
        vmm_map_range(kernel_pt, (vaddr_t)PHYS_TO_VIRT(base), base, end - base, fl);
    }

    struct limine_executable_address_response *ka = boot_kernel_address();
    uint64_t vbase = ka->virtual_base, pbase = ka->physical_base;
#define KMAP(s, e, fl) vmm_map_range(kernel_pt, (vaddr_t)(s), (paddr_t)(s) - vbase + pbase, \
                                     ALIGN_UP((uint64_t)(e), PAGE_SIZE) - (uint64_t)(s), fl)
    KMAP(__kernel_start, __text_start, VM_READ);
    KMAP(__text_start, __text_end, VM_READ | VM_EXEC);
    KMAP(__rodata_start, __rodata_end, VM_READ);
    KMAP(__data_start, __kernel_end, VM_READ | VM_WRITE);

    write_cr3(kernel_pt.root);
    pr_info("vmm: kernel page tables active (root %lx)\n", kernel_pt.root);
}
